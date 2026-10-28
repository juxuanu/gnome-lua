/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * run-dialog.c: The "Run a Command" dialog (Alt+F2). Port of
 * js/ui/runDialog.js.
 *
 * Differences from the JS module, other than the renames listed in
 * docs/PORT_STATUS.md:
 *
 *  - Of the internal commands (only available with the `development-tools`
 *    setting and without parental controls) `debugexit` and
 *    `check_cloexec_fds` are handled here. `lg` (open Looking Glass) and `rt`
 *    (reload the theme) need `Main.createLookingGlass ()` and
 *    `Main.loadTheme ()`, which are still JS: any other input is offered to
 *    the #ShellRunDialog::internal-command signal first, which main.js
 *    connects to. A handler returns %TRUE for the commands it knows; the
 *    input is run as a command line otherwise.
 *  - `Util.trySpawnCommandLine ()` (js/misc/util.js is still JS) is
 *    reimplemented here, including the rewriting of the error messages.
 *  - The error message of a file which no application can open is the
 *    message of the #GError as it is: the GJS prefix which the JS stripped
 *    does not exist.
 *  - The cursor is put after the completion by characters, the JS counted
 *    UTF-16 code units.
 *  - An unreadable directory in $PATH is reported with g_message () instead
 *    of `log ()`, as before.
 *  - `new Shell.RunDialog ()` takes no property for `dialog-style-class` and
 *    `destroy-on-close`: they are forced to "run-dialog" and %FALSE.
 */

#include "config.h"

#include <string.h>

#include <glib/gi18n-lib.h>
#include <meta/meta-context.h>

#define GNOME_DESKTOP_USE_UNSTABLE_API
#include <libgnome-desktop/gnome-systemd.h>

#include "misc/history.h"
#include "misc/parental-controls-manager.h"
#include "shell-global.h"
#include "shell-util.h"
#include "ui/dialog.h"
#include "ui/run-dialog.h"
#include "ui/shell-entry.h"

#define HISTORY_KEY "command-history"

#define LOCKDOWN_SCHEMA "org.gnome.desktop.lockdown"
#define DISABLE_COMMAND_LINE_KEY "disable-command-line"

#define TERMINAL_SCHEMA "org.gnome.desktop.default-applications.terminal"
#define EXEC_KEY "exec"
#define EXEC_ARG_KEY "exec-arg"

enum
{
  INTERNAL_COMMAND,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

struct _ShellRunDialog
{
  ShellModalDialog parent;

  ShellParentalControlsManager *parental_controls_manager; /* weak */
  GSettings *shell_settings; /* weak, owned by ShellGlobal */
  GSettings *lockdown_settings;
  GSettings *terminal_settings;
  gboolean enable_internal_commands;

  ClutterText *entry_text; /* weak */
  StLabel *description_label; /* weak */
  gboolean command_error;

  GFilenameCompleter *path_completer;
  ShellHistoryManager *history;
};

G_DEFINE_FINAL_TYPE (ShellRunDialog, shell_run_dialog, SHELL_TYPE_MODAL_DIALOG)

/* ------------------------------------------------------------------------ */
/* Completion                                                               */
/* ------------------------------------------------------------------------ */

/* The longest common prefix of two strings, by characters */
static char *
get_common (char       *s1,
            const char *s2)
{
  const char *p1, *p2;

  if (s1 == NULL)
    return g_strdup (s2);

  p1 = s1;
  p2 = s2;
  while (*p1 != '\0' && *p2 != '\0' &&
         g_utf8_get_char (p1) == g_utf8_get_char (p2))
    {
      p1 = g_utf8_next_char (p1);
      p2 = g_utf8_next_char (p2);
    }

  s1[p1 - s1] = '\0';
  return s1;
}

static void
collect_names_in_dir (const char *path,
                      const char *text,
                      GPtrArray  *results)
{
  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_autoptr (GFileEnumerator) file_enum = NULL;
  g_autoptr (GError) error = NULL;

  file_enum = g_file_enumerate_children (file, "standard::name",
                                         G_FILE_QUERY_INFO_NONE, NULL, &error);
  while (file_enum != NULL)
    {
      g_autoptr (GFileInfo) info = g_file_enumerator_next_file (file_enum, NULL, &error);
      const char *name;

      if (info == NULL)
        break;

      name = g_file_info_get_name (info);
      if (g_str_has_prefix (name, text))
        g_ptr_array_add (results, g_strdup (name));
    }

  if (error != NULL &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY))
    g_message ("%s: %s", path, error->message);
}

/* The rest of the longest command of $PATH or the home directory which
 * starts with @text, or %NULL if there is none */
static char *
get_command_completion (const char *text)
{
  g_autoptr (GPtrArray) results = g_ptr_array_new_with_free_func (g_free);
  g_auto (GStrv) paths = NULL;
  g_autofree char *common = NULL;
  const char *path_env = g_getenv ("PATH");
  guint i;

  if (path_env != NULL)
    paths = g_strsplit (path_env, ":", -1);

  for (i = 0; paths != NULL && paths[i] != NULL; i++)
    collect_names_in_dir (paths[i], text, results);
  collect_names_in_dir (g_get_home_dir (), text, results);

  if (results->len == 0)
    return NULL;

  for (i = 0; i < results->len; i++)
    common = get_common (g_steal_pointer (&common), g_ptr_array_index (results, i));

  return g_strdup (common + strlen (text));
}

static char *
get_completion (ShellRunDialog *self,
                const char     *text)
{
  if (strchr (text, '/') != NULL)
    return g_filename_completer_get_completion_suffix (self->path_completer, text);

  return get_command_completion (text);
}

/* ------------------------------------------------------------------------ */
/* Running a command                                                        */
/* ------------------------------------------------------------------------ */

static void
child_watch_cb (GPid     pid,
                int      wait_status,
                gpointer user_data)
{
}

/* Util.trySpawn (): runs @argv in the background */
static gboolean
try_spawn (const char * const  *argv,
           GError             **error)
{
  g_autoptr (GAppLaunchContext) launch_context = NULL;
  g_auto (GStrv) envp = NULL;
  g_autoptr (GError) spawn_error = NULL;
  GPid pid;

  launch_context = shell_global_create_app_launch_context (shell_global_get (), 0, -1);
  envp = g_app_launch_context_get_environment (launch_context);

  pid = shell_util_spawn_async (NULL, argv, (const char * const *) envp,
                                G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                &spawn_error);
  if (pid == 0)
    {
      /* Rewrite the error in case of ENOENT */
      if (g_error_matches (spawn_error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT))
        {
          g_set_error_literal (error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT,
                               _("Command not found"));
        }
      else
        {
          /* The error string is like:
           *   Failed to execute child process "foo" (No such file or directory)
           * We are only interested in the part in the parentheses. (And
           * we can't pattern match the text, since it gets localized.) */
          g_autoptr (GRegex) regex = g_regex_new (".*\\((.+)\\)", 0, 0, NULL);
          g_autofree char *message = NULL;

          if (regex != NULL)
            message = g_regex_replace (regex, spawn_error->message, -1, 0,
                                       "\\1", 0, NULL);
          g_set_error_literal (error, spawn_error->domain, spawn_error->code,
                               message != NULL ? message : spawn_error->message);
        }
      return FALSE;
    }

  /* Async call, we don't need the reply though */
  gnome_start_systemd_scope (argv[0], pid, NULL, NULL, NULL, NULL, NULL);

  /* Dummy child watch; we don't want to double-fork internally
   * because then we lose the parent-child relationship, which
   * can break polkit.  See https://bugzilla.redhat.com//show_bug.cgi?id=819275 */
  g_child_watch_add (pid, child_watch_cb, NULL);

  return TRUE;
}

/* Util.trySpawnCommandLine () */
static gboolean
try_spawn_command_line (const char  *command_line,
                        GError     **error)
{
  g_auto (GStrv) argv = NULL;

  if (!g_shell_parse_argv (command_line, NULL, &argv, error))
    return FALSE;

  return try_spawn ((const char * const *) argv, error);
}

static void
show_error (ShellRunDialog *self,
            const char     *message)
{
  self->command_error = TRUE;
  if (self->description_label != NULL)
    st_label_set_text (self->description_label, message);
}

/* The commands of the development tools */
static gboolean
run_internal_command (ShellRunDialog *self,
                      const char     *input)
{
  gboolean handled = FALSE;

  if (strcmp (input, "debugexit") == 0)
    {
      meta_context_terminate (shell_global_get_context (shell_global_get ()));
      return TRUE;
    }

  if (strcmp (input, "check_cloexec_fds") == 0)
    {
      shell_util_check_cloexec_fds ();
      return TRUE;
    }

  g_signal_emit (self, signals[INTERNAL_COMMAND], 0, input, &handled);

  return handled;
}

static void
run_command (ShellRunDialog *self,
             const char     *text,
             gboolean        in_terminal)
{
  g_autofree char *input = shell_history_manager_add_item (self->history, text); /* trims input */
  g_autofree char *command = NULL;
  g_autoptr (GError) error = NULL;

  self->command_error = FALSE;

  if (self->enable_internal_commands && run_internal_command (self, input))
    return;

  if (in_terminal)
    {
      g_autofree char *exec = g_settings_get_string (self->terminal_settings, EXEC_KEY);
      g_autofree char *exec_arg = g_settings_get_string (self->terminal_settings, EXEC_ARG_KEY);

      command = g_strdup_printf ("%s %s %s", exec, exec_arg, input);
    }
  else
    {
      command = g_strdup (input);
    }

  if (!try_spawn_command_line (command, &error))
    {
      /* Mmmh, that failed - see if @input matches an existing file */
      g_autofree char *path = NULL;

      if (input[0] == '/')
        {
          path = g_strdup (input);
        }
      else if (input[0] != '\0')
        {
          const char *name = input[0] == '~' ? input + 1 : input;

          path = g_strdup_printf ("%s/%s", g_get_home_dir (), name);
        }

      if (path != NULL && g_file_test (path, G_FILE_TEST_EXISTS))
        {
          g_autoptr (GFile) file = g_file_new_for_path (path);
          g_autofree char *uri = g_file_get_uri (file);
          g_autoptr (GAppLaunchContext) launch_context =
            shell_global_create_app_launch_context (shell_global_get (), 0, -1);
          g_autoptr (GError) launch_error = NULL;

          if (!g_app_info_launch_default_for_uri (uri, launch_context, &launch_error))
            show_error (self, launch_error->message);
        }
      else
        {
          show_error (self, error->message);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Dialog                                                                   */
/* ------------------------------------------------------------------------ */

static void
update_enable_internal_commands (ShellRunDialog *self)
{
  self->enable_internal_commands =
    !shell_parental_controls_manager_get_any_parental_controls_enabled (self->parental_controls_manager) &&
    g_settings_get_boolean (self->shell_settings, "development-tools");
}

static void
update_enable_internal_commands_cb (GObject    *object,
                                    GParamSpec *pspec,
                                    gpointer    user_data)
{
  update_enable_internal_commands (SHELL_RUN_DIALOG (user_data));
}

static void
text_changed_cb (ClutterText    *text,
                 ShellRunDialog *self)
{
  if (self->description_label != NULL)
    st_label_set_text (self->description_label, _("Press ESC to close"));
}

static gboolean
entry_key_press_cb (ClutterKeyController *controller,
                    ShellRunDialog       *self)
{
  ClutterModifierType pressed = 0, latched = 0, locked = 0;
  uint32_t symbol = 0;

  clutter_key_controller_get_key (controller, &symbol, NULL, NULL);
  clutter_key_controller_get_state (controller, &pressed, &latched, &locked);

  if (self->entry_text == NULL)
    return CLUTTER_EVENT_PROPAGATE;

  if (symbol == CLUTTER_KEY_Tab)
    {
      const char *text = clutter_text_get_text (self->entry_text);
      const char *space = strrchr (text, ' ');
      g_autofree char *prefix = g_strdup (space != NULL ? space + 1 : text);
      g_autofree char *postfix = get_completion (self, prefix);

      if (postfix != NULL && postfix[0] != '\0')
        {
          /* @text is the buffer of the entry: measure it before changing it */
          glong cursor = g_utf8_strlen (text, -1) + g_utf8_strlen (postfix, -1);

          clutter_text_insert_text (self->entry_text, postfix, -1);
          clutter_text_set_cursor_position (self->entry_text, cursor);
        }
      return CLUTTER_EVENT_STOP;
    }
  else if (symbol == CLUTTER_KEY_Return ||
           symbol == CLUTTER_KEY_KP_Enter ||
           symbol == CLUTTER_KEY_ISO_Enter)
    {
      g_autofree char *text = g_strdup (clutter_text_get_text (self->entry_text));
      ClutterModifierType state = pressed | latched | locked;

      shell_modal_dialog_pop_modal (SHELL_MODAL_DIALOG (self));
      run_command (self, text, (state & CLUTTER_CONTROL_MASK) != 0);
      if (!self->command_error ||
          !shell_modal_dialog_push_modal (SHELL_MODAL_DIALOG (self)))
        shell_modal_dialog_close (SHELL_MODAL_DIALOG (self));
      return CLUTTER_EVENT_STOP;
    }

  return CLUTTER_EVENT_PROPAGATE;
}

static gboolean
close_binding_cb (GObject             *object,
                  const char          *action_name,
                  guint                key_val,
                  ClutterModifierType  modifiers,
                  gpointer             user_data)
{
  shell_modal_dialog_close (SHELL_MODAL_DIALOG (object));
  return CLUTTER_EVENT_STOP;
}

static gboolean
shell_run_dialog_open (ShellModalDialog *dialog)
{
  ShellRunDialog *self = SHELL_RUN_DIALOG (dialog);

  shell_history_manager_last_item (self->history);
  if (self->entry_text != NULL)
    clutter_text_set_text (self->entry_text, "");
  self->command_error = FALSE;

  if (g_settings_get_boolean (self->lockdown_settings, DISABLE_COMMAND_LINE_KEY))
    return FALSE;

  return SHELL_MODAL_DIALOG_CLASS (shell_run_dialog_parent_class)->open (dialog);
}

/* dialog-style-class and destroy-on-close are construct-only properties of
 * the parent, which the JS class forced in `_init ()` */
static GObject *
shell_run_dialog_constructor (GType                  type,
                              guint                  n_construct_properties,
                              GObjectConstructParam *construct_properties)
{
  guint i;

  for (i = 0; i < n_construct_properties; i++)
    {
      const char *name = g_param_spec_get_name (construct_properties[i].pspec);

      if (strcmp (name, "dialog-style-class") == 0)
        g_value_set_string (construct_properties[i].value, "run-dialog");
      else if (strcmp (name, "destroy-on-close") == 0)
        g_value_set_boolean (construct_properties[i].value, FALSE);
    }

  return G_OBJECT_CLASS (shell_run_dialog_parent_class)->constructor (type,
                                                                      n_construct_properties,
                                                                      construct_properties);
}

static void
shell_run_dialog_constructed (GObject *object)
{
  ShellRunDialog *self = SHELL_RUN_DIALOG (object);
  ShellModalDialog *dialog = SHELL_MODAL_DIALOG (self);
  ShellGlobal *global = shell_global_get ();
  ShellMessageDialogContent *content;
  ClutterActor *label_actor;
  ClutterActor *entry;
  ClutterText *entry_text;
  ClutterAction *entry_key_controller;
  StLabel *description_label;

  G_OBJECT_CLASS (shell_run_dialog_parent_class)->constructed (object);

  self->parental_controls_manager = shell_parental_controls_manager_get_default ();
  g_object_add_weak_pointer (G_OBJECT (self->parental_controls_manager),
                             (gpointer *) &self->parental_controls_manager);
  g_signal_connect_object (self->parental_controls_manager,
                           "notify::any-parental-controls-enabled",
                           G_CALLBACK (update_enable_internal_commands_cb), self, 0);

  self->lockdown_settings = g_settings_new (LOCKDOWN_SCHEMA);
  self->terminal_settings = g_settings_new (TERMINAL_SCHEMA);
  self->shell_settings = shell_global_get_settings (global);
  g_object_add_weak_pointer (G_OBJECT (self->shell_settings),
                             (gpointer *) &self->shell_settings);
  g_signal_connect_object (self->shell_settings, "changed::development-tools",
                           G_CALLBACK (update_enable_internal_commands_cb), self, 0);
  update_enable_internal_commands (self);

  content = g_object_new (SHELL_TYPE_MESSAGE_DIALOG_CONTENT,
                          "title", _("Run a Command"),
                          NULL);
  clutter_actor_add_child (CLUTTER_ACTOR (shell_modal_dialog_get_content_layout (dialog)),
                           CLUTTER_ACTOR (content));
  label_actor = clutter_actor_get_first_child (CLUTTER_ACTOR (content));

  entry = g_object_new (ST_TYPE_ENTRY,
                        "style-class", "run-dialog-entry",
                        "input-purpose", CLUTTER_INPUT_CONTENT_PURPOSE_TERMINAL,
                        "label-actor", label_actor,
                        "can-focus", TRUE,
                        NULL);
  shell_entry_add_context_menu (ST_ENTRY (entry), SHELL_ACTION_MODE_POPUP);

  entry_text = st_entry_get_clutter_text (ST_ENTRY (entry));
  g_set_weak_pointer (&self->entry_text, entry_text);
  clutter_text_set_activatable (entry_text, FALSE);
  clutter_actor_add_child (CLUTTER_ACTOR (content), entry);
  shell_modal_dialog_set_initial_key_focus (dialog, CLUTTER_ACTOR (entry_text));

  description_label = g_object_new (ST_TYPE_LABEL,
                                    "style-class", "run-dialog-description",
                                    "text", _("Press ESC to close"),
                                    NULL);
  g_set_weak_pointer (&self->description_label, description_label);
  clutter_actor_add_child (CLUTTER_ACTOR (content), CLUTTER_ACTOR (description_label));

  self->command_error = FALSE;

  self->path_completer = g_filename_completer_new ();

  self->history = shell_history_manager_new (HISTORY_KEY, entry_text);

  entry_key_controller = clutter_key_controller_new (NULL);
  g_signal_connect_object (entry_key_controller, "key-press",
                           G_CALLBACK (entry_key_press_cb), self, 0);
  clutter_actor_add_action (CLUTTER_ACTOR (entry_text), entry_key_controller);

  g_signal_connect_object (entry_text, "text-changed",
                           G_CALLBACK (text_changed_cb), self, 0);
}

static void
shell_run_dialog_dispose (GObject *object)
{
  ShellRunDialog *self = SHELL_RUN_DIALOG (object);

  if (self->parental_controls_manager != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->parental_controls_manager, self);
      g_object_remove_weak_pointer (G_OBJECT (self->parental_controls_manager),
                                    (gpointer *) &self->parental_controls_manager);
      self->parental_controls_manager = NULL;
    }
  if (self->shell_settings != NULL)
    {
      g_signal_handlers_disconnect_by_data (self->shell_settings, self);
      g_object_remove_weak_pointer (G_OBJECT (self->shell_settings),
                                    (gpointer *) &self->shell_settings);
      self->shell_settings = NULL;
    }

  g_clear_object (&self->lockdown_settings);
  g_clear_object (&self->terminal_settings);
  g_clear_object (&self->path_completer);
  g_clear_object (&self->history);
  g_clear_weak_pointer (&self->entry_text);
  g_clear_weak_pointer (&self->description_label);

  G_OBJECT_CLASS (shell_run_dialog_parent_class)->dispose (object);
}

static void
shell_run_dialog_class_init (ShellRunDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ShellModalDialogClass *dialog_class = SHELL_MODAL_DIALOG_CLASS (klass);
  ClutterBindingPool *binding_pool;

  object_class->constructor = shell_run_dialog_constructor;
  object_class->constructed = shell_run_dialog_constructed;
  object_class->dispose = shell_run_dialog_dispose;

  dialog_class->open = shell_run_dialog_open;

  /**
   * ShellRunDialog::internal-command:
   * @self: the #ShellRunDialog
   * @name: the text which was entered
   *
   * Emitted when a command is run while the internal commands of the
   * development tools are enabled and @name is not one of the ones which the
   * dialog handles itself (`debugexit` and `check_cloexec_fds`). main.js
   * handles `lg` (open Looking Glass) and `rt` (reload the theme), which need
   * JS-only code.
   *
   * Returns: %TRUE if @name is an internal command, which stops the dialog
   *   from running it as a command line
   */
  signals[INTERNAL_COMMAND] =
    g_signal_new ("internal-command",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  g_signal_accumulator_true_handled, NULL, NULL,
                  G_TYPE_BOOLEAN, 1, G_TYPE_STRING);

  binding_pool = clutter_binding_pool_get_for_class (klass);
  clutter_binding_pool_install_action (binding_pool, "close",
                                       CLUTTER_KEY_Escape, CLUTTER_RELEASE_MASK,
                                       G_CALLBACK (close_binding_cb),
                                       NULL, NULL);
}

static void
shell_run_dialog_init (ShellRunDialog *self)
{
}

/**
 * shell_run_dialog_new:
 *
 * Creates the run dialog (`new RunDialog ()`). It stays alive when closed:
 * keep it and call shell_modal_dialog_open () to show it again.
 *
 * Returns: (transfer full): a new #ShellRunDialog
 */
ShellRunDialog *
shell_run_dialog_new (void)
{
  return g_object_new (SHELL_TYPE_RUN_DIALOG, NULL);
}
