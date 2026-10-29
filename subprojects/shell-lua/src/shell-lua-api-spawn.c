/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.spawn (docs/api/spawn.md): running a program with GSubprocess, gated
 * by the "spawn" capability.
 *
 * Notes for maintainers:
 *
 *  - A running process is an owned object of the runtime
 *    (shell_lua_own_object): its shell.Process handle is alive while the
 *    process runs; the slot is freed (and the handle dies) before on_exit
 *    runs; the destroy rule terminates the process (SIGTERM, SIGKILL two
 *    seconds later) and drops on_exit, so nothing runs after disable.
 *  - The output is read here, not with g_subprocess_communicate (): that
 *    collects without a bound. Each stream is read in chunks into a buffer of
 *    at most max_output bytes; the rest is read and thrown away so that the
 *    process never blocks on a full pipe.
 *  - The result is ready when the process has exited and both streams are at
 *    EOF and the standard input is written. A descendant that keeps a pipe
 *    open must not hold the result back for ever: GRACE_MS after the process
 *    exited, the result is delivered with what was read.
 *  - luaL_error () longjmps: nothing allocated may be live when it is called.
 *    The options are validated completely (nothing allocated) before they
 *    are copied.
 */

#ifndef G_LOG_DOMAIN
#define G_LOG_DOMAIN "ShellLua"
#endif

#include <signal.h>
#include <string.h>

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib-unix.h>

#include "shell-lua-api-private.h"

#define DEFAULT_MAX_OUTPUT (1024 * 1024)
#define MAX_MAX_OUTPUT (64 * 1024 * 1024)
#define MAX_STDIN (16 * 1024 * 1024)
#define MAX_TIMEOUT_MS 86400000
#define MAX_ARGS 4096
#define READ_CHUNK 65536
#define GRACE_MS 500
#define KILL_AFTER_MS 2000

static void __attribute__ ((constructor))
shell_lua_api_spawn_init (void)
{
  shell_lua_api_register ("spawn", shell_lua_api_spawn_register);
}

static void
release_ref (ShellLuaState *state,
             int           *ref)
{
  int r = *ref;

  *ref = LUA_NOREF;
  if (r != LUA_NOREF && r != LUA_REFNIL && state != NULL && shell_lua_state_is_alive (state))
    shell_lua_state_unref (state, r);
}

/* ------------------------------------------------------------------------ */
/* Terminating a process                                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
  GSubprocess *subprocess;
  guint        timer;
} Reaper;

static void
reaper_free (Reaper *reaper)
{
  g_clear_handle_id (&reaper->timer, g_source_remove);
  g_object_unref (reaper->subprocess);
  g_free (reaper);
}

static gboolean
reaper_kill (gpointer data)
{
  Reaper *reaper = data;

  reaper->timer = 0;
  g_subprocess_force_exit (reaper->subprocess);
  return G_SOURCE_REMOVE;
}

static void
reaper_waited (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
  g_subprocess_wait_finish (G_SUBPROCESS (source), result, NULL);
  reaper_free (user_data);
}

/* SIGTERM now, SIGKILL after KILL_AFTER_MS if it is still there. Owns what it
 * needs: the caller may drop the process object at once. */
static void
terminate_subprocess (GSubprocess *subprocess)
{
  Reaper *reaper;

  if (g_subprocess_get_identifier (subprocess) == NULL)
    return;

  g_subprocess_send_signal (subprocess, SIGTERM);

  reaper = g_new0 (Reaper, 1);
  reaper->subprocess = g_object_ref (subprocess);
  reaper->timer = g_timeout_add (KILL_AFTER_MS, reaper_kill, reaper);
  g_subprocess_wait_async (subprocess, NULL, reaper_waited, reaper);
}

/* ------------------------------------------------------------------------ */
/* ShellLuaProcess                                                           */
/* ------------------------------------------------------------------------ */

typedef struct _ShellLuaProcess ShellLuaProcess;

typedef struct {
  ShellLuaProcess *process;   /* unowned: the operations in flight hold references */
  GInputStream    *stream;    /* unowned: the subprocess has it */
  GByteArray      *buffer;
  gboolean         eof;
} Pipe;

#define SHELL_TYPE_LUA_PROCESS (shell_lua_process_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaProcess, shell_lua_process, SHELL, LUA_PROCESS, GObject)

struct _ShellLuaProcess
{
  GObject parent_instance;

  ShellLuaState *state;        /* unowned; NULL once destroyed */
  GSubprocess   *subprocess;
  GCancellable  *cancellable;
  int            pid;
  int            exit_ref;
  gboolean       finished;     /* result delivered, or destroyed */
  gboolean       exited;
  gboolean       timed_out;
  gboolean       truncated;
  gsize          max_output;
  guint          timeout_id;
  guint          grace_id;

  gboolean       stdin_done;
  GBytes        *stdin_bytes;

  Pipe           out;
  Pipe           err;
};

G_DEFINE_FINAL_TYPE (ShellLuaProcess, shell_lua_process, G_TYPE_OBJECT)

enum { PROCESS_PROP_0, PROCESS_PROP_PID, PROCESS_N_PROPS };
static GParamSpec *process_props[PROCESS_N_PROPS];

static void
shell_lua_process_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  ShellLuaProcess *process = SHELL_LUA_PROCESS (object);

  switch (prop_id)
    {
    case PROCESS_PROP_PID:
      g_value_set_int (value, process->pid);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
process_clear_sources (ShellLuaProcess *process)
{
  g_clear_handle_id (&process->timeout_id, g_source_remove);
  g_clear_handle_id (&process->grace_id, g_source_remove);
}

static void
shell_lua_process_finalize (GObject *object)
{
  ShellLuaProcess *process = SHELL_LUA_PROCESS (object);

  process_clear_sources (process);
  g_clear_object (&process->subprocess);
  g_clear_object (&process->cancellable);
  g_clear_pointer (&process->stdin_bytes, g_bytes_unref);
  g_clear_pointer (&process->out.buffer, g_byte_array_unref);
  g_clear_pointer (&process->err.buffer, g_byte_array_unref);

  G_OBJECT_CLASS (shell_lua_process_parent_class)->finalize (object);
}

static void
shell_lua_process_class_init (ShellLuaProcessClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = shell_lua_process_get_property;
  object_class->finalize = shell_lua_process_finalize;

  process_props[PROCESS_PROP_PID] =
    g_param_spec_int ("pid", NULL, NULL, 0, G_MAXINT, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, PROCESS_N_PROPS, process_props);
}

static void
shell_lua_process_init (ShellLuaProcess *process)
{
  process->exit_ref = LUA_NOREF;
  process->cancellable = g_cancellable_new ();
  process->out.process = process;
  process->err.process = process;
}

/* The destroy rule: terminate the process if it is still running, drop
 * on_exit, stop everything that is pending. */
static void
process_destroy_op (GObject *object)
{
  ShellLuaProcess *process = SHELL_LUA_PROCESS (object);
  ShellLuaState *state = process->state;

  process->finished = TRUE;
  process->state = NULL;
  process_clear_sources (process);
  g_cancellable_cancel (process->cancellable);
  if (process->subprocess != NULL && !process->exited)
    terminate_subprocess (process->subprocess);
  release_ref (state, &process->exit_ref);
}

static const ShellLuaOwnOps process_ops = { .destroy = process_destroy_op };

/* --- the result ------------------------------------------------------------ */

static const char *
signal_name (int  sig,
             char *buf,
             gsize size)
{
  switch (sig)
    {
    case SIGHUP: return "SIGHUP";
    case SIGINT: return "SIGINT";
    case SIGQUIT: return "SIGQUIT";
    case SIGILL: return "SIGILL";
    case SIGABRT: return "SIGABRT";
    case SIGFPE: return "SIGFPE";
    case SIGKILL: return "SIGKILL";
    case SIGSEGV: return "SIGSEGV";
    case SIGPIPE: return "SIGPIPE";
    case SIGALRM: return "SIGALRM";
    case SIGTERM: return "SIGTERM";
    case SIGUSR1: return "SIGUSR1";
    case SIGUSR2: return "SIGUSR2";
    case SIGBUS: return "SIGBUS";
    default:
      g_snprintf (buf, size, "SIG%d", sig);
      return buf;
    }
}

typedef struct {
  gboolean    exited;
  int         status;
  int         signal;
  GByteArray *out;
  GByteArray *err;
  gboolean    timed_out;
  gboolean    truncated;
} Result;

static void
push_bytes (lua_State  *L,
            GByteArray *bytes)
{
  lua_pushlstring (L, bytes != NULL ? (const char *) bytes->data : "", bytes != NULL ? bytes->len : 0);
}

static void
push_result (lua_State *L,
             gpointer   data)
{
  Result *result = data;
  char buf[24];

  luaL_checkstack (L, 6, "shell.spawn result");
  lua_createtable (L, 0, 6);
  if (result->exited)
    {
      lua_pushinteger (L, result->status);
      lua_setfield (L, -2, "status");
    }
  else
    {
      lua_pushstring (L, signal_name (result->signal, buf, sizeof buf));
      lua_setfield (L, -2, "signal");
    }
  push_bytes (L, result->out);
  lua_setfield (L, -2, "stdout");
  push_bytes (L, result->err);
  lua_setfield (L, -2, "stderr");
  lua_pushboolean (L, result->timed_out);
  lua_setfield (L, -2, "timed_out");
  lua_pushboolean (L, result->truncated);
  lua_setfield (L, -2, "truncated");
}

/* Ends @process: hands its slot back (the handle dies), then runs on_exit. */
static void
process_finish (ShellLuaProcess *process)
{
  g_autoptr(ShellLuaState) state = NULL;
  Result result = { 0 };
  int exit_ref;

  if (process->finished || process->state == NULL)
    return;

  state = g_object_ref (process->state);
  g_object_ref (process);

  result.exited = g_subprocess_get_if_exited (process->subprocess);
  result.status = result.exited ? g_subprocess_get_exit_status (process->subprocess) : 0;
  result.signal = g_subprocess_get_if_signaled (process->subprocess)
    ? g_subprocess_get_term_sig (process->subprocess) : 0;
  result.out = process->out.buffer;
  result.err = process->err.buffer;
  result.timed_out = process->timed_out;
  result.truncated = process->truncated;

  exit_ref = process->exit_ref;
  process->exit_ref = LUA_NOREF;
  process->finished = TRUE;

  if (shell_lua_state_is_alive (state))
    shell_lua_destroy_object (shell_lua_state_get_L (state), G_OBJECT (process));
  else
    process_destroy_op (G_OBJECT (process));

  if (exit_ref != LUA_NOREF && shell_lua_state_is_alive (state))
    shell_lua_call_ref (state, exit_ref, 1, push_result, &result);
  release_ref (state, &exit_ref);

  g_object_unref (process);
}

static void
process_check_done (ShellLuaProcess *process);

static gboolean
process_grace_expired (gpointer data)
{
  ShellLuaProcess *process = data;

  process->grace_id = 0;
  g_cancellable_cancel (process->cancellable);
  process_finish (process);
  return G_SOURCE_REMOVE;
}

/* The result when everything is in; when only the process has exited, wait a
 * little for the rest. Called whenever one of the conditions changed. */
static void
process_check_done (ShellLuaProcess *process)
{
  if (process->finished || !process->exited)
    return;

  if (process->out.eof && process->err.eof && process->stdin_done)
    {
      process_finish (process);
      return;
    }

  /* armed once: a descendant that never stops writing cannot hold it back */
  if (process->grace_id == 0)
    process->grace_id = g_timeout_add (GRACE_MS, process_grace_expired, process);
}

/* --- waiting --------------------------------------------------------------- */

static void
process_waited_cb (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  ShellLuaProcess *process = user_data;
  g_autoptr(GError) error = NULL;

  if (g_subprocess_wait_finish (G_SUBPROCESS (source), result, &error) && !process->finished)
    {
      process->exited = TRUE;
      g_clear_handle_id (&process->timeout_id, g_source_remove);
      process_check_done (process);
    }
  g_object_unref (process);
}

/* --- output ---------------------------------------------------------------- */

static void pipe_read_next (Pipe *pipe);

static void
pipe_read_cb (GObject      *source,
              GAsyncResult *result,
              gpointer      user_data)
{
  Pipe *pipe = user_data;
  ShellLuaProcess *process = pipe->process;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = g_input_stream_read_bytes_finish (G_INPUT_STREAM (source), result, &error);

  if (!process->finished)
    {
      gsize size = bytes != NULL ? g_bytes_get_size (bytes) : 0;

      if (bytes == NULL || size == 0)
        {
          /* EOF, or a stream that cannot be read any more */
          pipe->eof = TRUE;
          process_check_done (process);
        }
      else
        {
          gsize room = process->max_output > pipe->buffer->len ? process->max_output - pipe->buffer->len : 0;

          g_byte_array_append (pipe->buffer, g_bytes_get_data (bytes, NULL), MIN (size, room));
          if (size > room)
            process->truncated = TRUE;
          pipe_read_next (pipe);
        }
    }
  g_object_unref (process);
}

static void
pipe_read_next (Pipe *pipe)
{
  g_input_stream_read_bytes_async (pipe->stream, READ_CHUNK, G_PRIORITY_DEFAULT,
                                   pipe->process->cancellable, pipe_read_cb, pipe);
  g_object_ref (pipe->process);
}

static void
pipe_start (Pipe        *pipe,
            GInputStream *stream)
{
  pipe->stream = stream;
  pipe->buffer = g_byte_array_new ();
  pipe_read_next (pipe);
}

/* --- standard input -------------------------------------------------------- */

static void
stdin_closed_cb (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  ShellLuaProcess *process = user_data;

  g_output_stream_close_finish (G_OUTPUT_STREAM (source), result, NULL);
  if (!process->finished)
    {
      process->stdin_done = TRUE;
      process_check_done (process);
    }
  g_object_unref (process);
}

static void
stdin_written_cb (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  ShellLuaProcess *process = user_data;

  /* a process that does not read its input gives a broken pipe: not an error
   * of ours */
  g_output_stream_write_all_finish (G_OUTPUT_STREAM (source), result, NULL, NULL);
  g_output_stream_close_async (G_OUTPUT_STREAM (source), G_PRIORITY_DEFAULT, NULL,
                               stdin_closed_cb, g_object_ref (process));
  g_object_unref (process);
}

/* --- timeout --------------------------------------------------------------- */

static gboolean
process_timed_out_cb (gpointer data)
{
  ShellLuaProcess *process = data;

  process->timeout_id = 0;
  if (!process->finished && !process->exited)
    {
      process->timed_out = TRUE;
      terminate_subprocess (process->subprocess);
    }
  return G_SOURCE_REMOVE;
}

/* --- Lua side -------------------------------------------------------------- */

static int
l_process_kill (lua_State *L)
{
  GObject *object = shell_lua_handle_get (L, 1, "shell.Process");

  if (object != NULL)
    {
      ShellLuaProcess *process = SHELL_LUA_PROCESS (object);

      if (!process->exited)
        g_subprocess_send_signal (process->subprocess, SIGTERM);
    }
  return 0;
}

static const luaL_Reg process_methods[] = {
  { NULL, NULL }
};

static const ShellLuaHandleMethod process_methods_ex[] = {
  { "kill",    l_process_kill,                  SHELL_LUA_METHOD_DEAD_OK },
  { "destroy", shell_lua_handle_destroy_method, SHELL_LUA_METHOD_DEAD_OK },
  { NULL }
};

static const ShellLuaHandleProperty process_properties[] = {
  { "pid", "pid", TRUE, FALSE },
  { NULL }
};

static ShellLuaHandleType process_type = {
  .name = "shell.Process",
  .methods = process_methods,
  .methods_ex = process_methods_ex,
  .properties = process_properties,
};

static const char * const spawn_fields[] = {
  "argv", "cwd", "env", "stdin", "timeout_ms", "max_output", "on_exit", NULL
};

/* Raises unless every element of the table at @idx is a string without a NUL
 * byte; @what names the table for the message. Nothing allocated. */
static void
check_string_table (lua_State  *L,
                    int         idx,
                    const char *what)
{
  lua_Unsigned n = lua_rawlen (L, idx);
  lua_Unsigned i;

  if (n < 1)
    luaL_error (L, "field '%s' must not be empty", what);
  if (n > MAX_ARGS)
    luaL_error (L, "field '%s' has too many elements (at most %d)", what, MAX_ARGS);

  for (i = 1; i <= n; i++)
    {
      size_t len;
      const char *s;

      if (lua_rawgeti (L, idx, (lua_Integer) i) != LUA_TSTRING)
        luaL_error (L, "field '%s'[%d] must be a string, got %s", what, (int) i, luaL_typename (L, -1));
      s = lua_tolstring (L, -1, &len);
      if (strlen (s) != len)
        luaL_error (L, "field '%s'[%d] must not contain a NUL byte", what, (int) i);
      lua_pop (L, 1);
    }
}

/* Same for the NAME = "value" table. */
static void
check_env_table (lua_State *L,
                 int        idx)
{
  lua_pushnil (L);
  while (lua_next (L, idx) != 0)
    {
      size_t klen, vlen;
      const char *key, *value;

      if (lua_type (L, -2) != LUA_TSTRING)
        luaL_error (L, "field 'env' must have string keys, got a %s key", luaL_typename (L, -2));
      key = lua_tolstring (L, -2, &klen);
      if (klen == 0 || strlen (key) != klen || strchr (key, '=') != NULL)
        luaL_error (L, "field 'env': '%s' is not a valid variable name", key);
      if (lua_type (L, -1) != LUA_TSTRING)
        luaL_error (L, "field 'env.%s' must be a string, got %s", key, luaL_typename (L, -1));
      value = lua_tolstring (L, -1, &vlen);
      if (strlen (value) != vlen)
        luaL_error (L, "field 'env.%s' must not contain a NUL byte", key);
      lua_pop (L, 1);
    }
}

static int
l_spawn (lua_State *L)
{
  ShellLuaState *state = shell_lua_state_from_L (L);
  GSubprocessFlags flags = G_SUBPROCESS_FLAGS_NONE;
  GSubprocessLauncher *launcher;
  ShellLuaProcess *process;
  GSubprocess *subprocess;
  GError *error = NULL;
  const char *cwd = NULL;
  const char *input = NULL;
  size_t input_len = 0;
  lua_Integer timeout_ms, max_output;
  gboolean has_exit;
  char **argv;
  lua_Unsigned n, i;
  int argv_idx, cwd_idx, env_idx, stdin_idx, exit_idx;
  int exit_ref;

  shell_lua_require_capability (L, "spawn", "shell.spawn()");
  shell_lua_check_fields (L, 1, spawn_fields);

  /* 1. read every field once (this is where __index of the options table may
   * run Lua code), then validate the values on the stack; nothing is
   * allocated yet */
  timeout_ms = shell_lua_opt_integer (L, 1, "timeout_ms", G_MININT64);
  max_output = shell_lua_opt_integer (L, 1, "max_output", DEFAULT_MAX_OUTPUT);
  lua_getfield (L, 1, "argv");
  argv_idx = lua_gettop (L);
  lua_getfield (L, 1, "cwd");
  cwd_idx = lua_gettop (L);
  lua_getfield (L, 1, "env");
  env_idx = lua_gettop (L);
  lua_getfield (L, 1, "stdin");
  stdin_idx = lua_gettop (L);
  lua_getfield (L, 1, "on_exit");
  exit_idx = lua_gettop (L);

  if (lua_isnil (L, argv_idx))
    return luaL_error (L, "missing required field 'argv' (a sequence of strings)");
  if (lua_type (L, argv_idx) != LUA_TTABLE)
    return luaL_error (L, "field 'argv' must be a sequence of strings, got %s",
                       luaL_typename (L, argv_idx));
  check_string_table (L, argv_idx, "argv");

  if (!lua_isnil (L, cwd_idx))
    {
      size_t len;

      if (lua_type (L, cwd_idx) != LUA_TSTRING)
        return luaL_error (L, "field 'cwd' must be a string, got %s", luaL_typename (L, cwd_idx));
      cwd = lua_tolstring (L, cwd_idx, &len);
      if (len == 0 || strlen (cwd) != len)
        return luaL_error (L, "field 'cwd' must be a non-empty path without a NUL byte");
    }

  if (!lua_isnil (L, env_idx))
    {
      if (lua_type (L, env_idx) != LUA_TTABLE)
        return luaL_error (L, "field 'env' must be a table, got %s", luaL_typename (L, env_idx));
      check_env_table (L, env_idx);
    }

  if (!lua_isnil (L, stdin_idx))
    {
      if (lua_type (L, stdin_idx) != LUA_TSTRING)
        return luaL_error (L, "field 'stdin' must be a string, got %s", luaL_typename (L, stdin_idx));
      input = lua_tolstring (L, stdin_idx, &input_len);
      if (input_len > MAX_STDIN)
        return luaL_error (L, "field 'stdin' is too large (at most %d bytes)", MAX_STDIN);
    }

  if (timeout_ms != G_MININT64 && (timeout_ms < 1 || timeout_ms > MAX_TIMEOUT_MS))
    return luaL_error (L, "field 'timeout_ms' must be between 1 and %d", MAX_TIMEOUT_MS);
  if (max_output < 0 || max_output > MAX_MAX_OUTPUT)
    return luaL_error (L, "field 'max_output' must be between 0 and %d", MAX_MAX_OUTPUT);

  has_exit = !lua_isnil (L, exit_idx);
  if (has_exit && lua_type (L, exit_idx) != LUA_TFUNCTION &&
      luaL_getmetafield (L, exit_idx, "__call") == LUA_TNIL)
    return luaL_error (L, "field 'on_exit' must be a function, got %s", luaL_typename (L, exit_idx));

  shell_lua_check_owned_capacity (L);

  /* 2. copy and start. Everything is read raw from here on: no Lua code runs
   * until the process is owned. */
  n = lua_rawlen (L, argv_idx);
  argv = g_new0 (char *, n + 1);
  for (i = 0; i < n; i++)
    {
      lua_rawgeti (L, argv_idx, (lua_Integer) i + 1);
      argv[i] = g_strdup (lua_tostring (L, -1));
      lua_pop (L, 1);
    }

  if (has_exit)
    flags |= G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE;
  else
    flags |= G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE;
  if (input != NULL)
    flags |= G_SUBPROCESS_FLAGS_STDIN_PIPE;

  launcher = g_subprocess_launcher_new (flags);
  if (cwd != NULL)
    g_subprocess_launcher_set_cwd (launcher, cwd);

  if (!lua_isnil (L, env_idx))
    {
      lua_pushnil (L);
      while (lua_next (L, env_idx) != 0)
        {
          g_subprocess_launcher_setenv (launcher, lua_tostring (L, -2), lua_tostring (L, -1), TRUE);
          lua_pop (L, 1);
        }
    }

  subprocess = g_subprocess_launcher_spawnv (launcher, (const char * const *) argv, &error);
  g_object_unref (launcher);
  g_strfreev (argv);

  if (subprocess == NULL)
    {
      lua_pushnil (L);
      lua_pushstring (L, error->message);
      g_error_free (error);
      return 2;
    }

  exit_ref = LUA_NOREF;
  if (has_exit)
    {
      lua_pushvalue (L, exit_idx);
      exit_ref = luaL_ref (L, LUA_REGISTRYINDEX);
    }

  process = g_object_new (SHELL_TYPE_LUA_PROCESS, NULL);
  process->state = state;
  process->subprocess = subprocess;
  process->pid = g_subprocess_get_identifier (subprocess) != NULL
    ? (int) g_ascii_strtoll (g_subprocess_get_identifier (subprocess), NULL, 10) : 0;
  process->exit_ref = exit_ref;
  process->max_output = (gsize) max_output;
  process->stdin_done = (input == NULL);
  shell_lua_own_object (L, G_OBJECT (process), &process_ops);
  g_object_unref (process);   /* the owner holds it now */

  g_subprocess_wait_async (subprocess, process->cancellable, process_waited_cb,
                          g_object_ref (process));

  if (has_exit)
    {
      pipe_start (&process->out, g_subprocess_get_stdout_pipe (subprocess));
      pipe_start (&process->err, g_subprocess_get_stderr_pipe (subprocess));
    }
  else
    {
      process->out.eof = TRUE;
      process->err.eof = TRUE;
    }

  if (input != NULL)
    {
      GOutputStream *stream = g_subprocess_get_stdin_pipe (subprocess);

      /* GIO writes to a pipe that is ready in one write (), which blocks
       * the main loop for as long as the process does not read when the
       * descriptor is not non-blocking, and then neither of us can go on */
      if (G_IS_UNIX_OUTPUT_STREAM (stream))
        g_unix_set_fd_nonblocking (g_unix_output_stream_get_fd (G_UNIX_OUTPUT_STREAM (stream)), TRUE, NULL);

      if (input_len > 0)
        {
          process->stdin_bytes = g_bytes_new (input, input_len);
          g_output_stream_write_all_async (stream, g_bytes_get_data (process->stdin_bytes, NULL),
                                           input_len, G_PRIORITY_DEFAULT, process->cancellable,
                                           stdin_written_cb, g_object_ref (process));
        }
      else
        {
          g_output_stream_close_async (stream, G_PRIORITY_DEFAULT, NULL, stdin_closed_cb,
                                       g_object_ref (process));
        }
    }

  if (timeout_ms != G_MININT64)
    process->timeout_id = g_timeout_add ((guint) timeout_ms, process_timed_out_cb, process);

  shell_lua_handle_push (L, "shell.Process", G_OBJECT (process));
  return 1;
}

void
shell_lua_api_spawn_register (ShellLuaState *state,
                              lua_State     *L)
{
  int shell = lua_absindex (L, -1);

  luaL_checkstack (L, 4, "shell spawn api");

  process_type.gtype = SHELL_TYPE_LUA_PROCESS;
  shell_lua_handle_register_type (L, &process_type);

  lua_pushcfunction (L, l_spawn);
  lua_setfield (L, shell, "spawn");
}
