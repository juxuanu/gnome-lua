/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

#define _GNU_SOURCE

#include "config.h"

#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/common_interface_defs.h>
#endif

#if defined (HAVE_MALLINFO) || defined (HAVE_MALLINFO2)
#include <malloc.h>
#endif
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <clutter/clutter.h>
#include <glib-unix.h>
#include <glib/gi18n-lib.h>
#include <meta/meta-context.h>
#include <meta/meta-plugin.h>
#include <meta/prefs.h>
#include <atk-bridge.h>

#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#include "shell-global.h"
#include "shell-global-private.h"
#include "shell-lua.h"
#include "shell-perf-log.h"
#include "shell-test-runner.h"
#include "st.h"
#include "ui/main.h"
#include "ui/session-mode.h"

extern GType gnome_shell_plugin_get_type (void);

#define SHELL_DBUS_SERVICE "org.gnome.Shell"

#define WM_NAME "GNOME Shell"
#define GNOME_WM_KEYBINDINGS "Mutter,GNOME Shell"

static gboolean is_gdm_mode = FALSE;
static char *session_mode = NULL;
static int caught_signal = 0;
static gboolean force_animations = FALSE;
static char *lua_extension_path = NULL;

#define DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER 1
#define DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER 4

enum {
  SHELL_DEBUG_BACKTRACE_WARNINGS = 1,
  SHELL_DEBUG_BACKTRACE_SEGFAULTS = 2,
};
static int _shell_debug;
static gboolean _tracked_signals[NSIG] = { 0 };
static GThread *main_thread;

static void
shell_dbus_acquire_name (GDBusProxy  *bus,
                         guint32      request_name_flags,
                         guint32     *request_name_result,
                         const gchar *name,
                         gboolean     fatal)
{
  GError *error = NULL;
  GVariant *request_name_variant;

  if (!(request_name_variant = g_dbus_proxy_call_sync (bus,
                                                       "RequestName",
                                                       g_variant_new ("(su)", name, request_name_flags),
                                                       0, /* call flags */
                                                       -1, /* timeout */
                                                       NULL, /* cancellable */
                                                       &error)))
    {
      g_printerr ("failed to acquire %s: %s\n", name, error->message);
      g_clear_error (&error);
      if (!fatal)
        return;
      exit (1);
    }
  g_variant_get (request_name_variant, "(u)", request_name_result);
  g_variant_unref (request_name_variant);
}

static void
shell_dbus_init (void)
{
  GDBusConnection *session;
  GDBusProxy *bus;
  GError *error = NULL;
  guint32 request_name_flags;
  guint32 request_name_result;

  session = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);

  if (error) {
    g_printerr ("Failed to connect to session bus: %s\n", error->message);
    exit (1);
  }

  bus = g_dbus_proxy_new_sync (session,
                               G_DBUS_PROXY_FLAGS_NONE,
                               NULL, /* interface info */
                               "org.freedesktop.DBus",
                               "/org/freedesktop/DBus",
                               "org.freedesktop.DBus",
                               NULL, /* cancellable */
                               &error);

  if (!bus)
    {
      g_printerr ("Failed to get a session bus proxy: %s\n", error->message);
      exit (1);
    }

  request_name_flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;

  shell_dbus_acquire_name (bus,
                           request_name_flags,
                           &request_name_result,
                           SHELL_DBUS_SERVICE, TRUE);
  if (!(request_name_result == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER
        || request_name_result == DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER))
    {
      g_printerr (SHELL_DBUS_SERVICE " already exists on bus\n");
      exit (1);
    }

  g_object_unref (bus);
  g_object_unref (session);
}

static void
malloc_statistics_callback (ShellPerfLog *perf_log,
                            gpointer      data)
{
#if defined (HAVE_MALLINFO) || defined (HAVE_MALLINFO2)
#ifdef HAVE_MALLINFO2
  struct mallinfo2 info = mallinfo2 ();
#else
  struct mallinfo info = mallinfo ();
#endif

  shell_perf_log_update_statistic_i (perf_log,
                                     "malloc.arenaSize",
                                     info.arena);
  shell_perf_log_update_statistic_i (perf_log,
                                     "malloc.mmapSize",
                                     info.hblkhd);
  shell_perf_log_update_statistic_i (perf_log,
                                     "malloc.usedSize",
                                     info.uordblks);
#endif /* defined (HAVE_MALLINFO) || defined (HAVE_MALLINFO2) */
}

static void
shell_perf_log_init (void)
{
  ShellPerfLog *perf_log = shell_perf_log_get_default ();

  /* For probably historical reasons, mallinfo() defines the returned values,
   * even those in bytes as int, not size_t. We're determined not to use
   * more than 2G of malloc'ed memory, so are OK with that.
   */
  shell_perf_log_define_statistic (perf_log,
                                   "malloc.arenaSize",
                                   "Amount of memory allocated by malloc() with brk(), in bytes",
                                   "i");
  shell_perf_log_define_statistic (perf_log,
                                   "malloc.mmapSize",
                                   "Amount of memory allocated by malloc() with mmap(), in bytes",
                                   "i");
  shell_perf_log_define_statistic (perf_log,
                                   "malloc.usedSize",
                                   "Amount of malloc'ed memory currently in use",
                                   "i");

  shell_perf_log_add_statistics_callback (perf_log,
                                          malloc_statistics_callback,
                                          NULL, NULL);
}

static void
shell_a11y_init (void)
{
  if (clutter_get_accessibility_enabled () == FALSE)
    {
      g_warning ("Accessibility: clutter has no accessibility enabled"
                 " skipping the atk-bridge load");
    }
  else
    {
      atk_bridge_adaptor_init (NULL, NULL);
    }
}

static void
shell_init_debug (const char *debug_env)
{
  static const GDebugKey keys[] = {
    { "backtrace-warnings", SHELL_DEBUG_BACKTRACE_WARNINGS },
    { "backtrace-segfaults", SHELL_DEBUG_BACKTRACE_SEGFAULTS },
  };

  _shell_debug = g_parse_debug_string (debug_env, keys,
                                       G_N_ELEMENTS (keys));
}

/*
 * Prints the stack of the calling thread to stderr: the Lua traceback of the
 * extension state that is running, if there is one, then the C stack (the
 * sanitizer's under AddressSanitizer, which symbolizes it, otherwise
 * backtrace_symbols_fd ()). It only uses write () and, after
 * dump_stack_init (), does not allocate, so a fatal signal handler can call
 * it. This replaces gjs_dumpstack ().
 */
static void
dump_stack (void)
{
  /* Lua extensions (gnome-lua): does not allocate; prints nothing if no Lua
   * code is running */
  shell_lua_dump_stack ();

#ifdef __SANITIZE_ADDRESS__
  /* The C side of a fatal warning (gnome-lua): the sanitizer symbolizes
   * the stack of the frame that raised it */
  __sanitizer_print_stack_trace ();
#elif defined (HAVE_EXECINFO_H)
  {
    static const char header[] = "== C stack ==\n";
    void *frames[128];
    ssize_t ignored;
    int n_frames;

    ignored = write (STDERR_FILENO, header, sizeof header - 1);
    (void) ignored;

    n_frames = backtrace (frames, G_N_ELEMENTS (frames));
    backtrace_symbols_fd (frames, n_frames, STDERR_FILENO);
  }
#endif
}

/* The first call of backtrace () loads libgcc, which allocates: do it before
 * a signal handler has to */
static void
dump_stack_init (void)
{
#if !defined (__SANITIZE_ADDRESS__) && defined (HAVE_EXECINFO_H)
  void *frame;

  backtrace (&frame, 1);
#endif
}

static GLogWriterOutput
default_log_writer (GLogLevelFlags   log_level,
                    const GLogField *fields,
                    gsize            n_fields,
                    gpointer         user_data)
{
  GLogWriterOutput output;

  output = g_log_writer_default (log_level, fields, n_fields, user_data);

  if ((_shell_debug & SHELL_DEBUG_BACKTRACE_WARNINGS) &&
      ((log_level & G_LOG_LEVEL_CRITICAL) ||
       (log_level & G_LOG_LEVEL_WARNING)) &&
      g_thread_self () == main_thread)
    dump_stack ();

  return output;
}

static GLogWriterOutput
shut_up (GLogLevelFlags   log_level,
         const GLogField *fields,
         gsize            n_fields,
         gpointer         user_data)
{
  return (GLogWriterOutput) {0};
}

static void
dump_stack_alarm_sigaction (int signo)
{
  g_log_set_writer_func (g_log_writer_default, NULL, NULL);
  g_warning ("Failed to dump the stack, got stuck");
  g_log_set_writer_func (default_log_writer, NULL, NULL);

  raise (caught_signal);
}

static void
dump_stack_on_signal_handler (int signo)
{
  struct sigaction sa = { .sa_handler = dump_stack_alarm_sigaction };
  gsize i;

  /* Ignore all the signals starting this point, a part the one we'll raise
   * (which is implicitly ignored here through SA_RESETHAND), this is needed
   * not to get this handler being called by other signals that we were
   * tracking and that might be emitted by code called starting from now.
   */
  for (i = 0; i < G_N_ELEMENTS (_tracked_signals); ++i)
    {
      if (_tracked_signals[i] && i != signo)
        signal (i, SIG_IGN);
    }

  /* Waiting at least 5 seconds for the dumpstack, if it fails, we raise the error */
  caught_signal = signo;
  sigemptyset (&sa.sa_mask);
  sigaction (SIGALRM, &sa, NULL);

  alarm (5);
  dump_stack ();
  alarm (0);

  raise (signo);
}

static void
dump_stack_on_signal (int signo)
{
  struct sigaction sa = {
    .sa_flags   = SA_RESETHAND | SA_NODEFER,
    .sa_handler = dump_stack_on_signal_handler,
  };

  sigemptyset (&sa.sa_mask);

  sigaction (signo, &sa, NULL);
  _tracked_signals[signo] = TRUE;
}

static gboolean
list_modes (const char  *option_name,
            const char  *value,
            gpointer     data,
            GError     **error)
{
  g_auto (GStrv) modes = NULL;
  int i;

  g_log_set_writer_func (shut_up, NULL, NULL);

  modes = shell_session_mode_list_modes ();
  for (i = 0; modes[i] != NULL; i++)
    g_print ("%s\n", modes[i]);

  exit (0);
}

static gboolean
print_version (const gchar    *option_name,
               const gchar    *value,
               gpointer        data,
               GError        **error)
{
  g_print ("GNOME Shell %s\n", VERSION);
  exit (0);
}

GOptionEntry gnome_shell_options[] = {
  {
    "version", 0, G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
    print_version,
    N_("Print version"),
    NULL
  },
  {
    "gdm-mode", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE,
    &is_gdm_mode,
    N_("Mode used by GDM for login screen"),
    NULL
  },
  {
    "mode", 0, 0, G_OPTION_ARG_STRING,
    &session_mode,
    N_("Use a specific mode, e.g. “gdm” for login screen"),
    "MODE"
  },
  {
    "list-modes", 0, G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
    list_modes,
    N_("List possible modes"),
    NULL
  },
  {
    "force-animations", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
    &force_animations,
    N_("Force animations to be enabled"),
    NULL
  },
  {
    "lua-extension-path", 0, 0, G_OPTION_ARG_FILENAME,
    &lua_extension_path,
    N_("Additional directories to search for Lua extensions"),
    "PATH"
  },
  { NULL }
};

static gboolean
on_sigterm (gpointer user_data)
{
  MetaContext *context = META_CONTEXT (user_data);

  meta_context_terminate (context);

  return G_SOURCE_REMOVE;
}

static void
init_signal_handlers (MetaContext *context)
{
  struct sigaction act = { 0 };
  sigset_t empty_mask;

  sigemptyset (&empty_mask);
  act.sa_handler = SIG_IGN;
  act.sa_mask = empty_mask;
  act.sa_flags = 0;
  if (sigaction (SIGPIPE,  &act, NULL) < 0)
    g_warning ("Failed to register SIGPIPE handler: %s", g_strerror (errno));
#ifdef SIGXFSZ
  if (sigaction (SIGXFSZ,  &act, NULL) < 0)
    g_warning ("Failed to register SIGXFSZ handler: %s", g_strerror (errno));
#endif

  g_unix_signal_add (SIGINT, on_sigterm, context);
  g_unix_signal_add (SIGTERM, on_sigterm, context);
}

/* Lua extensions (gnome-lua): the extension manager reads
 * SHELL_LUA_EXTENSION_PATH (a list of directories, searched before the
 * standard ones) when it is created, so this must run before any JS does.
 * Relative entries are resolved against the directory we were started in. */
static void
setup_lua_extension_path (const char *cwd)
{
  g_autoptr (GString) resolved = NULL;
  g_auto (GStrv) entries = NULL;
  int i;

  if (lua_extension_path == NULL || *lua_extension_path == '\0')
    return;

  entries = g_strsplit (lua_extension_path, G_SEARCHPATH_SEPARATOR_S, -1);
  resolved = g_string_new (NULL);
  for (i = 0; entries[i]; i++)
    {
      g_autofree char *entry = NULL;

      if (*entries[i] == '\0')
        continue;

      entry = g_canonicalize_filename (entries[i], cwd);
      if (resolved->len > 0)
        g_string_append (resolved, G_SEARCHPATH_SEPARATOR_S);
      g_string_append (resolved, entry);
    }

  if (resolved->len > 0)
    g_setenv ("SHELL_LUA_EXTENSION_PATH", resolved->str, TRUE);
}

static void
change_to_home_directory (void)
{
  const char *home_dir;

  home_dir = g_get_home_dir ();
  if (!home_dir)
    return;

  if (chdir (home_dir) < 0)
    g_warning ("Could not change to home directory %s", home_dir);
}

/*
 * Starts the shell once the main loop runs (what init.js queued with
 * GLib.idle_add ()): creates the singletons and the UI. If that fails the
 * main loop ends with the error, which main () reports.
 */
static gboolean
start_shell_cb (gpointer user_data)
{
  MetaContext *context = user_data;
  GError *error = NULL;

  if (!shell_main_start (shell_main_get_default (), &error))
    meta_context_terminate_with_error (context, error);

  return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
  g_autoptr (MetaContext) context = NULL;
  g_autofree char *cwd = NULL;
  GError *error = NULL;
  int ecode = EXIT_SUCCESS;

  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  main_thread = g_thread_self ();

  context = meta_create_context (WM_NAME);
  meta_context_add_option_entries (context, gnome_shell_options,
                                   GETTEXT_PACKAGE);

  session_mode = (char *) g_getenv ("GNOME_SHELL_SESSION_MODE");

  if (!meta_context_configure (context, &argc, &argv, &error))
    {
      g_printerr ("Failed to configure: %s\n", error->message);
      return EXIT_FAILURE;
    }

  meta_context_set_plugin_gtype (context, gnome_shell_plugin_get_type ());
  meta_context_set_gnome_wm_keybindings (context, GNOME_WM_KEYBINDINGS);

  init_signal_handlers (context);
  cwd = g_get_current_dir ();
  setup_lua_extension_path (cwd);
  change_to_home_directory ();

  if (session_mode == NULL)
    session_mode = is_gdm_mode ? (char *)"gdm" : (char *)"user";

  dump_stack_init ();
  dump_stack_on_signal (SIGABRT);
  dump_stack_on_signal (SIGFPE);
  dump_stack_on_signal (SIGIOT);
  dump_stack_on_signal (SIGTRAP);

  if ((_shell_debug & SHELL_DEBUG_BACKTRACE_SEGFAULTS))
    {
      dump_stack_on_signal (SIGBUS);
      dump_stack_on_signal (SIGSEGV);
    }

  /* Initialize the Shell global */
  _shell_global_init ("session-mode", session_mode,
                      "force-animations", force_animations,
                      NULL);

  if (!meta_context_setup (context, &error))
    {
      g_printerr ("Failed to setup: %s\n", error->message);
      return EXIT_FAILURE;
    }

  shell_init_debug (g_getenv ("SHELL_DEBUG"));

  shell_dbus_init ();
  shell_a11y_init ();
  shell_perf_log_init ();

  /* Ensure the GNOME Shell cursor implementation registers itself as
   * a GIO extension.
   */
  g_type_ensure (ST_TYPE_CURSOR);

  g_log_set_writer_func (default_log_writer, NULL, NULL);

  meta_context_raise_rlimit_nofile (context, NULL);

  if (!meta_context_start (context, &error))
    {
      g_printerr ("GNOME Shell failed to start: %s\n", error->message);
      return EXIT_FAILURE;
    }

  shell_test_runner_start (); /* runs $SHELL_TEST_SCRIPT, if set, once the UI is built */

  /* Queue starting the shell, then run the main loop of the Meta context
   * (what init.js did); it returns when Mutter is exited. */
  g_idle_add_full (G_PRIORITY_DEFAULT, start_shell_cb, context, NULL);

  if (!meta_context_run_main_loop (context, &error))
    {
      g_message ("GNOME Shell terminated with an error: %s", error->message);
      g_error_free (error);
      /* We just exit() here, since in a development environment you'll get the
       * error in your shell output, and it's way better than a busted WM,
       * which typically manifests as a white screen.
       *
       * In production, we shouldn't crash =)  But if we do, we should get
       * restarted by the session infrastructure, which is likely going
       * to be better than some undefined state.
       */
      exit (1);
    }

  g_message ("Shutting down GNOME Shell");
  _shell_global_notify_shutdown (shell_global_get ());

  /* Lua extensions (gnome-lua): disable them while the UI is alive */
  g_debug ("Tearing down Lua extensions");
  _shell_global_shutdown_lua (shell_global_get ());

  g_debug ("Tearing down the shell");
  shell_main_shutdown (shell_main_get_default ());
  g_object_unref (shell_global_get ());

  g_debug ("Tearing down the mutter context");
  meta_context_destroy (g_steal_pointer (&context));

  return ecode;
}
