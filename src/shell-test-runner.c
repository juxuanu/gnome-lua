/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * shell-test-runner.c: runs the Lua test script named by SHELL_TEST_SCRIPT
 * (the Lua files of tests/shell) inside the shell. It replaces the automation
 * scripts of the JS shell (js/ui/scripting.js and the JS modules of tests/shell).
 *
 * The script runs in a privileged Lua state, a ShellLuaInspector of its own
 * (the one of Looking Glass is not touched): full standard library, generic
 * handles for GObjects (`obj.some_property`, `obj.some_property = value`,
 * dashes of the property names are underscores), and these globals:
 *
 *   global, stage, main   ShellGlobal, its stage and ShellMain: the
 *                         singletons of the shell are reached through
 *                         `main` (main.panel, main.overview,
 *                         main.layout_manager, main.message_tray,
 *                         main.window_manager ...)
 *   print                 writes to stdout
 *   test                  the module described below
 *
 * and, on the handles, the methods the tests need (obj:connect (name, fn),
 * obj:disconnect (id), main.overview:show (), panel:navigate_focus (...)
 * ...: see the method tables at the end of this file).
 *
 * The script is a coroutine. These functions of `test` wait, by yielding it
 * until the main loop has done what was asked; they can only be called by
 * the script itself, not by a callback (signal handler or timeout):
 *
 *   test.sleep (ms)               sleep (Scripting.sleep)
 *   test.wait_leisure ()          until the shell is idle (waitLeisure)
 *   test.wait_startup ()          until the startup of the shell is done:
 *                                 the end of the layout manager's
 *                                 startup-complete (the script starts
 *                                 before it, as the init () function of the
 *                                 JS tests ran)
 *   test.watch (obj, signal)      starts listening to a signal; returns a
 *                                 waiter
 *   test.wait (waiter [, ms])     until the signal was emitted (at once if
 *                                 it was already); an error after @ms
 *   test.wait_signal (obj, signal [, ms])  watch and wait
 *   test.wait_for (predicate, description [, ms])  polls until true
 *   test.wait_and_draw (ms)       redraws the stage at every frame for @ms
 *   test.create_window {width, height, alpha, maximized, redraws,
 *                       text_input}
 *                                 creates a window with
 *                                 gnome-shell-perf-helper (which is
 *                                 started the first time)
 *   test.wait_windows ()          until the windows are mapped and drawn
 *   test.destroy_windows ()       destroys them
 *
 * Tests of the extension API (tests/api, see tests/api/README.md). The shell
 * the runner drives has a test-only `shell.test` table in the sandbox of
 * every extension (report (name, ok [, message]), done ([label])), and the
 * hook `test.step` (payload `step:s` and what the script adds), which the
 * script emits with shell.hooks.emit_after () to run a step of a fixture
 * extension. The script controls and observes the extensions with:
 *
 *   test.extension_enable (uuid)  adds the extension to enabled-extensions
 *                                 (it starts from the main loop: wait for
 *                                 its state) and forgets its report
 *   test.extension_disable (uuid) removes it from enabled-extensions
 *   test.extension_state (uuid)   {state = "active" ..., error = string or
 *                                 nil, errors = number of Lua errors since
 *                                 it was enabled}, nil if not installed
 *   test.extension_report (uuid)  {checks = {{name, ok, message}...},
 *                                 passed, failed, done = {label = true}}
 *   test.log_capture (uuid, on)   captures the log of the extension
 *                                 (domain shell-lua/<uuid>, and the errors
 *                                 of its callbacks) and makes warnings
 *                                 non-fatal; with on = false it returns
 *                                 {{level, message}...}
 *   test.fingerprint ()           {actors = {type = n}, mapped = {type = n},
 *                                 listeners = {hook = n}}: what is on the
 *                                 stage and connected to the hooks
 *   test.http_server {routes = {["/path"] = body or route}}
 *                                 a SoupServer on 127.0.0.1 and an ephemeral
 *                                 port for the tests of shell.network (the
 *                                 CI has no internet): returns {url, port,
 *                                 route (), requests (), stop ()}; the
 *                                 routes are described at the function
 *
 * Other functions: test.timeout (ms, fn), test.now (), test.new (type,
 * {property = value ...}) (a GObject, kept alive by the runner),
 * test.new_monitor (width, height, refresh_rate) (a virtual monitor of
 * Mutter's test library; has :destroy ()), test.create_virtual_device
 * (device type), test.press_accelerator (accelerator) (types a combination
 * such as "<Super><Alt>j" on a virtual keyboard; it does not wait),
 * test.set_boolean (schema, key, value), test.expect_equal
 * (a, b, description), test.log (...), test.print (...); and for the
 * performance log (js/ui/scripting.js): test.define_event (name,
 * description [, signature]), test.event (name), test.event_i (name, int),
 * test.collect_statistics (). The events are `script.<name>`.
 *
 * When the script returns, the performance log is replayed to the handlers
 * the script registered with test.on_event ("script.name", function (time,
 * arg) ... end) (also for "clutter.stagePaintDone", "malloc.usedSize" ...)
 * or test.on_script_event ("name", fn), the functions registered with
 * test.finish (fn) are called, and the metrics (test.metrics (table), each
 * entry {description, units, value}) are written to SHELL_PERF_OUTPUT as
 * JSON or printed. An error anywhere (the script, a callback, a failed call
 * of the perf helper) makes the shell exit with an error.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <dlfcn.h>
#include <math.h>
#include <string.h>

#include <clutter/clutter.h>
#include <gio/gio.h>
#include <meta/display.h>
#include <meta/meta-backend.h>
#include <meta/meta-context.h>
#include <meta/window.h>
#include <libsoup/soup.h>

#include "shell-app-system.h"
#include "shell-app.h"
#include "shell-global.h"
#include "shell-hooks.h"
#include "shell-lua-api-private.h"
#include "shell-lua-extension-private.h"
#include "shell-lua-handle.h"
#include "shell-lua-inspector.h"
#include "shell-enum-types.h"
#include "shell-perf-log.h"
#include "shell-test-runner.h"
#include "st.h"
/* The generated St enum types (ST_TYPE_DIRECTION_TYPE) are not part of st.h */
#define ST_H_INSIDE 1
#include "st-enum-types.h"
#undef ST_H_INSIDE
#include "ui/extension-system.h"
#include "ui/layout.h"
#include "ui/main.h"
#include "ui/message-tray.h"
#include "ui/overview.h"
#include "ui/panel.h"
#include "ui/popup-menu.h"

#ifndef MUTTER_TEST_LIBRARY
#define MUTTER_TEST_LIBRARY "libmutter-test-51.so"
#endif

#define SCRIPT_ENV "SHELL_TEST_SCRIPT"
#define MUTTER_TEST_LIBRARY_ENV "SHELL_TEST_MUTTER_TEST_LIBRARY"

#define POLL_INTERVAL_MS 20

#define PERF_HELPER_NAME "org.gnome.Shell.PerfHelper"
#define PERF_HELPER_PATH "/org/gnome/Shell/PerfHelper"

#define WAITER_METATABLE "ShellTestWaiter"
#define MONITOR_METATABLE "ShellTestMonitor"

typedef GObject * (* TestMonitorNewFunc)     (MetaContext *context,
                                              int          width,
                                              int          height,
                                              float        refresh_rate,
                                              GError     **error);
typedef void      (* TestMonitorDestroyFunc) (GObject     *monitor);

typedef struct
{
  GWeakRef  object;
  gulong    handler_id;
  gboolean  emitted;
  gboolean  waiting;
  gboolean  timed_out;
  guint     serial;
  char     *signal;
} Waiter;

typedef struct
{
  GObject *monitor; /* NULL once destroyed */
} TestMonitor;

typedef struct
{
  char *script;

  ShellLuaInspector *inspector;
  lua_State *L;   /* the main thread of the state */
  lua_State *co;  /* the thread of the script */
  int co_ref;

  guint poll_id;
  gulong shutdown_id;
  gulong startup_id;
  GWeakRef layout_manager;

  gboolean startup_complete;
  gboolean waiting_startup;
  gboolean finished;      /* the shell was told to terminate */

  /* The script is parked while it waits for something to resume it */
  gboolean parked;
  guint park_serial;
  guint wake_source;      /* the timeout that resumes it, if any */

  /* gnome-shell-perf-helper */
  GSubprocess *helper;
  guint helper_watch_id;
  gboolean helper_ready;
  gboolean helper_auto_exit;
  GDBusConnection *bus;
  GCancellable *cancellable;
  char *pending_method;
  GVariant *pending_params;
  guint pending_serial;

  /* Mutter's test library, for the virtual monitors */
  void *test_library;
  TestMonitorNewFunc monitor_new;
  TestMonitorDestroyFunc monitor_destroy;

  /* Tests of the extension API */
  GHashTable *reports;  /* uuid -> ExtensionReport */
  char *capture_uuid;
  guint capture_handler;       /* shell-lua/<uuid> */
  guint capture_lib_handler;   /* ShellLua, the messages of the runtime about the extension */
  GLogLevelFlags capture_fatal;
  GPtrArray *captured;  /* CapturedLog */

  GPtrArray *keep;      /* GObject, strong: what the script created */
  GPtrArray *monitors;  /* TestMonitor */
  GPtrArray *waiters;   /* Waiter */
} Runner;

static Runner *the_runner = NULL;

gboolean
shell_test_runner_is_enabled (void)
{
  const char *script = g_getenv (SCRIPT_ENV);

  return script != NULL && script[0] != '\0';
}

/* ------------------------------------------------------------------------ */
/* Ending the run                                                            */
/* ------------------------------------------------------------------------ */

static void
runner_finish (Runner *r,
               GError *error) /* (transfer full) */
{
  MetaContext *context;

  if (r->finished)
    return;

  r->finished = TRUE;
  context = shell_global_get_context (shell_global_get ());

  if (error != NULL)
    {
      g_printerr ("Test script %s failed: %s\n", r->script, error->message);
      meta_context_terminate_with_error (context, error); /* takes the error */
    }
  else
    {
      g_message ("Test script %s finished", r->script);
      meta_context_terminate (context);
    }
}

static void G_GNUC_PRINTF (2, 3)
runner_fail (Runner     *r,
             const char *format,
             ...)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *message = NULL;
  va_list args;

  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);

  error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, message);
  runner_finish (r, g_steal_pointer (&error));
}

/* ------------------------------------------------------------------------ */
/* Calling Lua                                                               */
/* ------------------------------------------------------------------------ */

/* A message with the traceback for the error on the top of the stack of the
 * dead thread @t. @L is any live thread of the state. */
static char *
thread_error_message (lua_State *L,
                      lua_State *t)
{
  const char *message;
  char *result;

  if (!lua_checkstack (L, 6) || !lua_checkstack (t, 4))
    return g_strdup ("error (out of stack space)");

  message = luaL_tolstring (t, -1, NULL);
  luaL_traceback (L, t, message, 0);
  result = g_strdup (lua_tostring (L, -1));
  lua_pop (L, 1);
  lua_pop (t, 2);
  return result;
}

/*
 * Calls the function that is below its @nargs arguments on top of the stack
 * of the main thread, in a thread of its own (so that it can fail and be
 * told off without touching the script). The function and the arguments are
 * popped. The (first) result is stored in @result if it is not NULL.
 * Returns FALSE and fails the run if the call raised an error or yielded.
 */
static gboolean
call_in_thread (Runner   *r,
                int       nargs,
                gboolean *result)
{
  lua_State *L = r->L;
  lua_State *t;
  int status, nres = 0;
  int function_index = lua_gettop (L) - nargs;
  gboolean ok = TRUE;

  t = lua_newthread (L);
  lua_insert (L, function_index);
  lua_xmove (L, t, nargs + 1);

  status = lua_resume (t, L, nargs, &nres);
  if (status == LUA_OK)
    {
      if (result != NULL)
        *result = nres > 0 && lua_toboolean (t, -nres);
    }
  else if (status == LUA_YIELD)
    {
      ok = FALSE;
      runner_fail (r, "A callback cannot wait: test.sleep () and the other waiting functions can only be called by the script itself");
    }
  else
    {
      g_autofree char *message = thread_error_message (L, t);

      ok = FALSE;
      runner_fail (r, "Error in a callback: %s", message);
    }

  lua_pop (L, 1);
  return ok;
}

static void resume_script (Runner *r);

/* Marks the script as waiting; the caller then arms what resumes it and
 * yields. Raises an error if @L is not the thread of the script. */
static guint
begin_park (lua_State  *L,
            const char *what)
{
  Runner *r = the_runner;

  if (r == NULL || L != r->co)
    luaL_error (L, "test.%s () can only be called by the script itself, not by a callback",
                what);

  r->parked = TRUE;
  r->park_serial++;
  return r->park_serial;
}

/* Resumes the script if it still waits for @serial */
static gboolean
wake_if_parked (guint serial)
{
  Runner *r = the_runner;

  if (r == NULL || r->L == NULL || !r->parked || serial != r->park_serial)
    return FALSE;

  resume_script (r);
  return TRUE;
}

static gboolean
wake_idle_cb (gpointer data)
{
  wake_if_parked (GPOINTER_TO_UINT (data));
  return G_SOURCE_REMOVE;
}

static gboolean
sleep_timeout_cb (gpointer data)
{
  Runner *r = the_runner;

  if (r != NULL && r->parked && GPOINTER_TO_UINT (data) == r->park_serial)
    {
      r->wake_source = 0;
      resume_script (r);
    }

  return G_SOURCE_REMOVE;
}

static void
script_finished (Runner *r);

static void
resume_script (Runner *r)
{
  int status, nres = 0;

  r->parked = FALSE;
  g_clear_handle_id (&r->wake_source, g_source_remove);

  status = lua_resume (r->co, r->L, 0, &nres);
  if (status == LUA_YIELD)
    {
      lua_pop (r->co, nres);
      if (!r->parked)
        runner_fail (r, "The script yielded without waiting for anything: use the waiting functions of the test module");
    }
  else if (status == LUA_OK)
    {
      script_finished (r);
    }
  else
    {
      g_autofree char *message = thread_error_message (r->L, r->co);

      runner_fail (r, "%s", message);
    }
}

/* ------------------------------------------------------------------------ */
/* Helpers for the functions of the module                                    */
/* ------------------------------------------------------------------------ */

static gpointer
check_instance (lua_State *L,
                int        idx,
                GType      type)
{
  GObject *object = shell_lua_handle_check (L, idx, NULL);

  if (!G_TYPE_CHECK_INSTANCE_TYPE (object, type))
    luaL_error (L, "bad argument #%d (%s expected, got %s)", idx,
                g_type_name (type), G_OBJECT_TYPE_NAME (object));

  return object;
}

static gpointer
opt_instance (lua_State *L,
              int        idx,
              GType      type)
{
  if (lua_isnoneornil (L, idx))
    return NULL;

  return check_instance (L, idx, type);
}

static void
push_object_or_nil (lua_State *L,
                    gpointer   object)
{
  if (object != NULL)
    shell_lua_inspector_push_object (L, G_OBJECT (object));
  else
    lua_pushnil (L);
}

/* The value of the enum with the nick at @idx */
static int
check_enum (lua_State *L,
            int        idx,
            GType      type)
{
  const char *nick = luaL_checkstring (L, idx);
  GEnumClass *klass = g_type_class_ref (type);
  GEnumValue *value = g_enum_get_value_by_nick (klass, nick);
  int result;

  if (value == NULL)
    {
      GString *valid = g_string_new (NULL);
      guint i;

      for (i = 0; i < klass->n_values; i++)
        g_string_append_printf (valid, "%s%s", i > 0 ? ", " : "", klass->values[i].value_nick);
      g_type_class_unref (klass);
      lua_pushfstring (L, "invalid %s '%s', expected one of: %s",
                       g_type_name (type), nick, valid->str);
      g_string_free (valid, TRUE);
      lua_error (L);
    }

  result = value->value;
  g_type_class_unref (klass);
  return result;
}

static void
push_rectangle (lua_State *L,
                lua_Number x,
                lua_Number y,
                lua_Number width,
                lua_Number height)
{
  lua_createtable (L, 0, 4);
  lua_pushnumber (L, x);
  lua_setfield (L, -2, "x");
  lua_pushnumber (L, y);
  lua_setfield (L, -2, "y");
  lua_pushnumber (L, width);
  lua_setfield (L, -2, "width");
  lua_pushnumber (L, height);
  lua_setfield (L, -2, "height");
}

static void
push_monitor (lua_State          *L,
              const ShellMonitor *monitor)
{
  if (monitor == NULL)
    {
      lua_pushnil (L);
      return;
    }

  push_rectangle (L, monitor->x, monitor->y, monitor->width, monitor->height);
  lua_pushinteger (L, monitor->index);
  lua_setfield (L, -2, "index");
  lua_pushnumber (L, monitor->geometry_scale);
  lua_setfield (L, -2, "geometry_scale");
}

static void
keep_object (Runner  *r,
             GObject *object)
{
  if (g_object_is_floating (object))
    g_object_ref_sink (object);
  else
    g_object_ref (object);

  g_ptr_array_add (r->keep, object);
}

/* ------------------------------------------------------------------------ */
/* Signals                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct
{
  GClosure closure;
  int ref;
} LuaClosure;

typedef struct
{
  int ref;
  const GValue *params;
  guint n_params;
} HandlerCall;

static void
push_signal_value (lua_State    *L,
                   const GValue *value)
{
  switch (G_TYPE_FUNDAMENTAL (G_VALUE_TYPE (value)))
    {
    case G_TYPE_BOOLEAN:
    case G_TYPE_CHAR:
    case G_TYPE_UCHAR:
    case G_TYPE_INT:
    case G_TYPE_UINT:
    case G_TYPE_LONG:
    case G_TYPE_ULONG:
    case G_TYPE_INT64:
    case G_TYPE_UINT64:
    case G_TYPE_FLOAT:
    case G_TYPE_DOUBLE:
    case G_TYPE_STRING:
    case G_TYPE_ENUM:
    case G_TYPE_FLAGS:
    case G_TYPE_OBJECT:
    case G_TYPE_INTERFACE:
      shell_lua_push_gvalue (L, value);
      break;

    default:
      /* boxed types and the like: the name of the type */
      lua_pushfstring (L, "<%s>", g_type_name (G_VALUE_TYPE (value)));
      break;
    }
}

/* Runs in the thread of the call: the errors raised here fail the call, not
 * the process */
static int
handler_trampoline (lua_State *L)
{
  HandlerCall *call = lua_touserdata (L, 1);
  guint i;

  luaL_checkstack (L, call->n_params + 4, "signal arguments");
  lua_rawgeti (L, LUA_REGISTRYINDEX, call->ref);
  for (i = 0; i < call->n_params; i++)
    push_signal_value (L, &call->params[i]);
  lua_call (L, call->n_params, 1);
  lua_pushboolean (L, lua_toboolean (L, -1));
  return 1;
}

static void
lua_closure_marshal (GClosure     *closure,
                     GValue       *return_value,
                     guint         n_param_values,
                     const GValue *param_values,
                     gpointer      invocation_hint,
                     gpointer      marshal_data)
{
  LuaClosure *lua_closure = (LuaClosure *) closure;
  Runner *r = the_runner;
  HandlerCall call;
  gboolean result = FALSE;

  if (r == NULL || r->L == NULL)
    return;

  call.ref = lua_closure->ref;
  call.params = param_values;
  call.n_params = n_param_values;

  if (!lua_checkstack (r->L, 4))
    return;

  lua_pushcfunction (r->L, handler_trampoline);
  lua_pushlightuserdata (r->L, &call);
  call_in_thread (r, 1, &result);

  /* A handler returns what a signal with a boolean result stops with; a
   * handler that returns nothing continues the emission */
  if (return_value != NULL && G_VALUE_HOLDS_BOOLEAN (return_value))
    g_value_set_boolean (return_value, result);
}

static void
lua_closure_finalized (gpointer data,
                       GClosure *closure)
{
  LuaClosure *lua_closure = (LuaClosure *) closure;
  Runner *r = the_runner;

  if (r != NULL && r->L != NULL)
    luaL_unref (r->L, LUA_REGISTRYINDEX, lua_closure->ref);
}

/* obj:connect (name, fn) -> id */
static int
l_object_connect (lua_State *L)
{
  GObject *object = shell_lua_handle_check (L, 1, NULL);
  const char *name = luaL_checkstring (L, 2);
  GClosure *closure;
  LuaClosure *lua_closure;
  guint signal_id;
  GQuark detail;
  gulong id;

  luaL_checktype (L, 3, LUA_TFUNCTION);

  if (!g_signal_parse_name (name, G_OBJECT_TYPE (object), &signal_id, &detail, TRUE))
    return luaL_error (L, "%s has no signal '%s'", G_OBJECT_TYPE_NAME (object), name);

  closure = g_closure_new_simple (sizeof (LuaClosure), NULL);
  lua_closure = (LuaClosure *) closure;
  lua_pushvalue (L, 3);
  lua_closure->ref = luaL_ref (L, LUA_REGISTRYINDEX);
  g_closure_add_finalize_notifier (closure, NULL, lua_closure_finalized);
  g_closure_set_marshal (closure, lua_closure_marshal);

  id = g_signal_connect_closure_by_id (object, signal_id, detail, closure, FALSE);
  lua_pushinteger (L, (lua_Integer) id);
  return 1;
}

/* obj:disconnect (id) */
static int
l_object_disconnect (lua_State *L)
{
  GObject *object = shell_lua_handle_check (L, 1, NULL);
  lua_Integer id = luaL_checkinteger (L, 2);

  if (id <= 0 || !g_signal_handler_is_connected (object, (gulong) id))
    return luaL_error (L, "no handler %I is connected to the object", id);

  g_signal_handler_disconnect (object, (gulong) id);
  return 0;
}

static const luaL_Reg object_methods[] = {
  { "connect", l_object_connect },
  { "disconnect", l_object_disconnect },
  { NULL, NULL }
};

/* Waiters: test.watch () and test.wait () */

static void
waiter_disconnect (Waiter *waiter)
{
  GObject *object;

  if (waiter->handler_id == 0)
    return;

  object = g_weak_ref_get (&waiter->object);
  if (object != NULL)
    {
      if (g_signal_handler_is_connected (object, waiter->handler_id))
        g_signal_handler_disconnect (object, waiter->handler_id);
      g_object_unref (object);
    }
  waiter->handler_id = 0;
}

static void
waiter_free (Waiter *waiter)
{
  waiter_disconnect (waiter);
  g_weak_ref_clear (&waiter->object);
  g_free (waiter->signal);
  g_free (waiter);
}

static void
waiter_marshal (GClosure     *closure,
                GValue       *return_value,
                guint         n_param_values,
                const GValue *param_values,
                gpointer      invocation_hint,
                gpointer      marshal_data)
{
  Waiter *waiter = closure->data;
  Runner *r = the_runner;

  if (r == NULL || waiter->emitted)
    return;

  waiter->emitted = TRUE;
  waiter_disconnect (waiter);

  /* The script goes on once the emitter is done, like a resolved promise */
  if (waiter->waiting && r->parked && waiter->serial == r->park_serial)
    g_idle_add (wake_idle_cb, GUINT_TO_POINTER (waiter->serial));
}

/* test.watch (object, signal) -> waiter */
static int
l_watch (lua_State *L)
{
  Runner *r = the_runner;
  GObject *object = shell_lua_handle_check (L, 1, NULL);
  const char *name = luaL_checkstring (L, 2);
  Waiter *waiter, **udata;
  GClosure *closure;
  guint signal_id;
  GQuark detail;

  if (!g_signal_parse_name (name, G_OBJECT_TYPE (object), &signal_id, &detail, TRUE))
    return luaL_error (L, "%s has no signal '%s'", G_OBJECT_TYPE_NAME (object), name);

  waiter = g_new0 (Waiter, 1);
  g_weak_ref_init (&waiter->object, object);
  waiter->signal = g_strdup (name);
  g_ptr_array_add (r->waiters, waiter);

  closure = g_closure_new_simple (sizeof (GClosure), waiter);
  g_closure_set_marshal (closure, waiter_marshal);
  waiter->handler_id = g_signal_connect_closure_by_id (object, signal_id, detail,
                                                       closure, FALSE);

  udata = lua_newuserdatauv (L, sizeof (Waiter *), 0);
  *udata = waiter;
  luaL_setmetatable (L, WAITER_METATABLE);
  return 1;
}

static int
wait_continue (lua_State   *L,
               int          status,
               lua_KContext ctx)
{
  Waiter *waiter = *(Waiter **) luaL_checkudata (L, 1, WAITER_METATABLE);

  waiter->waiting = FALSE;
  if (waiter->timed_out)
    {
      waiter->timed_out = FALSE;
      waiter_disconnect (waiter);
      return luaL_error (L, "Timeout waiting for the signal '%s'", waiter->signal);
    }

  return 0;
}

static gboolean
wait_timeout_cb (gpointer data)
{
  Runner *r = the_runner;
  guint serial = GPOINTER_TO_UINT (data);
  guint i;

  if (r == NULL || !r->parked || serial != r->park_serial)
    return G_SOURCE_REMOVE;

  r->wake_source = 0;
  for (i = 0; i < r->waiters->len; i++)
    {
      Waiter *waiter = g_ptr_array_index (r->waiters, i);

      if (waiter->waiting && waiter->serial == serial)
        waiter->timed_out = TRUE;
    }

  resume_script (r);
  return G_SOURCE_REMOVE;
}

/* test.wait (waiter [, timeout_ms]) */
static int
l_wait (lua_State *L)
{
  Runner *r = the_runner;
  Waiter *waiter = *(Waiter **) luaL_checkudata (L, 1, WAITER_METATABLE);
  lua_Integer timeout = luaL_optinteger (L, 2, 0);
  guint serial;

  if (waiter->emitted)
    return 0;

  serial = begin_park (L, "wait");
  waiter->waiting = TRUE;
  waiter->serial = serial;
  if (timeout > 0)
    r->wake_source = g_timeout_add ((guint) timeout, wait_timeout_cb, GUINT_TO_POINTER (serial));

  return lua_yieldk (L, 0, 0, wait_continue);
}

/* ------------------------------------------------------------------------ */
/* test.sleep (), test.wait_leisure (), test.wait_startup ()                  */
/* ------------------------------------------------------------------------ */

static int
l_sleep (lua_State *L)
{
  Runner *r = the_runner;
  lua_Number ms = luaL_checknumber (L, 1);
  guint serial = begin_park (L, "sleep");

  r->wake_source = g_timeout_add ((guint) MAX (ms, 0), sleep_timeout_cb,
                                  GUINT_TO_POINTER (serial));
  g_source_set_name_by_id (r->wake_source, "[gnome-shell] test sleep");
  return lua_yield (L, 0);
}

typedef struct
{
  ClutterTimeline *timeline;
  guint serial;
} DrawWait;

static void
draw_wait_new_frame_cb (ClutterTimeline *timeline,
                        int              msecs,
                        gpointer         user_data)
{
  ClutterStage *stage = shell_global_get_stage (shell_global_get ());

  clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));
}

static gboolean
draw_wait_done_cb (gpointer user_data)
{
  DrawWait *wait = user_data;

  wake_if_parked (wait->serial);
  g_object_unref (wait->timeline);
  g_free (wait);
  return G_SOURCE_REMOVE;
}

static void
draw_wait_completed_cb (ClutterTimeline *timeline,
                        gpointer         user_data)
{
  /* Finish outside of the emission of the timeline */
  clutter_timeline_stop (timeline);
  g_idle_add (draw_wait_done_cb, user_data);
}

/* test.wait_and_draw (ms): redraws the stage at every frame for @ms
 * milliseconds (the measurements of redraw times need a stage that paints) */
static int
l_wait_and_draw (lua_State *L)
{
  lua_Number ms = luaL_checknumber (L, 1);
  guint serial = begin_park (L, "wait_and_draw");
  ClutterStage *stage = shell_global_get_stage (shell_global_get ());
  DrawWait *wait = g_new0 (DrawWait, 1);

  wait->serial = serial;
  wait->timeline = clutter_timeline_new_for_actor (CLUTTER_ACTOR (stage), (guint) MAX (ms, 1));
  g_signal_connect (wait->timeline, "new-frame", G_CALLBACK (draw_wait_new_frame_cb), NULL);
  g_signal_connect (wait->timeline, "completed", G_CALLBACK (draw_wait_completed_cb), wait);
  clutter_timeline_start (wait->timeline);
  return lua_yield (L, 0);
}

static void
leisure_cb (gpointer data)
{
  wake_if_parked (GPOINTER_TO_UINT (data));
}

static int
l_wait_leisure (lua_State *L)
{
  guint serial = begin_park (L, "wait_leisure");

  shell_global_run_at_leisure (shell_global_get (), leisure_cb,
                               GUINT_TO_POINTER (serial), NULL);
  return lua_yield (L, 0);
}

static int
l_wait_startup (lua_State *L)
{
  Runner *r = the_runner;

  if (r->startup_complete)
    return 0;

  begin_park (L, "wait_startup");
  r->waiting_startup = TRUE;
  return lua_yield (L, 0);
}

static void
startup_complete_cb (ShellLayoutManager *layout_manager,
                     gpointer            user_data)
{
  Runner *r = user_data;

  r->startup_complete = TRUE;
  if (r->waiting_startup && r->parked)
    {
      r->waiting_startup = FALSE;
      g_idle_add (wake_idle_cb, GUINT_TO_POINTER (r->park_serial));
    }
}

/* ------------------------------------------------------------------------ */
/* test.timeout (), test.now (), test.expect_equal (), logging               */
/* ------------------------------------------------------------------------ */

typedef struct
{
  int ref;
} TimeoutCall;

static gboolean
timeout_call_cb (gpointer data)
{
  TimeoutCall *call = data;
  Runner *r = the_runner;

  if (r != NULL && r->L != NULL)
    {
      if (lua_checkstack (r->L, 4))
        {
          lua_rawgeti (r->L, LUA_REGISTRYINDEX, call->ref);
          call_in_thread (r, 0, NULL);
        }
      luaL_unref (r->L, LUA_REGISTRYINDEX, call->ref);
    }

  g_free (call);
  return G_SOURCE_REMOVE;
}

/* test.timeout (ms, fn): calls fn once after ms milliseconds */
static int
l_timeout (lua_State *L)
{
  lua_Number ms = luaL_checknumber (L, 1);
  TimeoutCall *call;

  luaL_checktype (L, 2, LUA_TFUNCTION);
  call = g_new0 (TimeoutCall, 1);
  lua_pushvalue (L, 2);
  call->ref = luaL_ref (L, LUA_REGISTRYINDEX);
  g_timeout_add ((guint) MAX (ms, 0), timeout_call_cb, call);
  return 0;
}

static int
l_now (lua_State *L)
{
  lua_pushinteger (L, g_get_monotonic_time () / 1000);
  return 1;
}

/* The arguments joined by tabs */
static const char *
join_arguments (lua_State *L)
{
  int n = lua_gettop (L);
  luaL_Buffer buffer;
  int i;

  luaL_buffinit (L, &buffer);
  for (i = 1; i <= n; i++)
    {
      luaL_tolstring (L, i, NULL);
      if (i > 1)
        luaL_addchar (&buffer, '\t');
      luaL_addvalue (&buffer);
    }
  luaL_pushresult (&buffer);
  return lua_tostring (L, -1);
}

static int
l_print (lua_State *L)
{
  g_print ("%s\n", join_arguments (L));
  return 0;
}

static int
l_log (lua_State *L)
{
  g_message ("%s", join_arguments (L));
  return 0;
}

/* ------------------------------------------------------------------------ */
/* The performance log                                                       */
/* ------------------------------------------------------------------------ */

/* test.define_event (name, description [, signature]) */
static int
l_define_event (lua_State *L)
{
  const char *name = luaL_checkstring (L, 1);
  const char *description = luaL_checkstring (L, 2);
  const char *signature = luaL_optstring (L, 3, "");
  g_autofree char *event = g_strconcat ("script.", name, NULL);

  shell_perf_log_define_event (shell_perf_log_get_default (), event, description, signature);
  return 0;
}

static int
l_event (lua_State *L)
{
  const char *name = luaL_checkstring (L, 1);
  g_autofree char *event = g_strconcat ("script.", name, NULL);

  shell_perf_log_event (shell_perf_log_get_default (), event);
  return 0;
}

static int
l_event_i (lua_State *L)
{
  const char *name = luaL_checkstring (L, 1);
  lua_Integer value = luaL_checkinteger (L, 2);
  g_autofree char *event = g_strconcat ("script.", name, NULL);

  shell_perf_log_event_i (shell_perf_log_get_default (), event, (gint32) value);
  return 0;
}

static int
l_collect_statistics (lua_State *L)
{
  shell_perf_log_collect_statistics (shell_perf_log_get_default ());
  return 0;
}

static void
replay_cb (gint64      time,
           const char *name,
           const char *signature,
           GValue     *arg,
           gpointer    user_data)
{
  lua_State *L = user_data;

  lua_createtable (L, 0, 3);
  lua_pushinteger (L, time);
  lua_setfield (L, -2, "time");
  lua_pushstring (L, name);
  lua_setfield (L, -2, "name");
  if (G_VALUE_HOLDS_INT (arg))
    lua_pushinteger (L, g_value_get_int (arg));
  else if (G_VALUE_HOLDS_INT64 (arg))
    lua_pushinteger (L, g_value_get_int64 (arg));
  else if (G_VALUE_HOLDS_STRING (arg) && g_value_get_string (arg) != NULL)
    lua_pushstring (L, g_value_get_string (arg));
  else
    lua_pushnil (L);
  lua_setfield (L, -2, "arg");
  lua_rawseti (L, -2, (lua_Integer) lua_rawlen (L, -2) + 1);
}

/* test.events (): the events of the performance log, an array of {time,
 * name, arg} */
static int
l_events (lua_State *L)
{
  lua_newtable (L);
  luaL_checkstack (L, 6, "events");
  shell_perf_log_replay (shell_perf_log_get_default (), replay_cb, L);
  return 1;
}

static gboolean
write_report (const char  *path,
              const char  *metrics,
              GError     **error)
{
  g_autoptr (GFile) file = g_file_new_for_path (path);
  g_autoptr (GFileOutputStream) raw = NULL;
  g_autoptr (GOutputStream) out = NULL;
  ShellPerfLog *perf_log = shell_perf_log_get_default ();
  ShellLayoutManager *layout_manager = shell_main_get_layout_manager (shell_main_get_default ());
  int n_monitors, primary, i;

  raw = g_file_replace (file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, error);
  if (raw == NULL)
    return FALSE;

  out = g_buffered_output_stream_new_sized (G_OUTPUT_STREAM (raw), 4096);

  if (!g_output_stream_write_all (out, "{\n\"events\":\n", 12, NULL, NULL, error) ||
      !shell_perf_log_dump_events (perf_log, out, error))
    return FALSE;

  n_monitors = shell_layout_manager_get_n_monitors (layout_manager);
  primary = shell_layout_manager_get_primary_index (layout_manager);
  if (!g_output_stream_write_all (out, ",\n\"monitors\":\n[", 15, NULL, NULL, error))
    return FALSE;

  for (i = 0; i < n_monitors; i++)
    {
      ShellMonitor *monitor = shell_layout_manager_get_monitor (layout_manager, i);
      g_autofree char *entry = NULL;
      gboolean ok;

      entry = g_strdup_printf ("%s\"%s%dx%d+%d+%d\"", i != 0 ? ", " : "",
                               i == primary ? "*" : "",
                               monitor->width, monitor->height, monitor->x, monitor->y);
      shell_monitor_free (monitor);
      ok = g_output_stream_write_all (out, entry, strlen (entry), NULL, NULL, error);
      if (!ok)
        return FALSE;
    }

  if (!g_output_stream_write_all (out, " ]", 2, NULL, NULL, error) ||
      !g_output_stream_write_all (out, ",\n\"metrics\":\n[ ", 15, NULL, NULL, error) ||
      !g_output_stream_write_all (out, metrics, strlen (metrics), NULL, NULL, error) ||
      !g_output_stream_write_all (out, " ]", 2, NULL, NULL, error) ||
      !g_output_stream_write_all (out, ",\n\"log\":\n", 9, NULL, NULL, error) ||
      !shell_perf_log_dump_log (perf_log, out, error) ||
      !g_output_stream_write_all (out, "\n}\n", 3, NULL, NULL, error))
    return FALSE;

  return g_output_stream_close (out, NULL, error);
}

/* test.write_report (path, metrics): the JSON report that
 * gnome-shell-test-tool reads: the events, the monitors, the metrics (@metrics
 * is the text of their JSON objects) and the log */
static int
l_write_report (lua_State *L)
{
  const char *path = luaL_checkstring (L, 1);
  const char *metrics = luaL_checkstring (L, 2);
  g_autoptr (GError) error = NULL;

  if (!write_report (path, metrics, &error))
    {
      lua_pushfstring (L, "Could not write %s: %s", path, error->message);
      g_clear_error (&error);
      return lua_error (L);
    }

  return 0;
}

/* ------------------------------------------------------------------------ */
/* gnome-shell-perf-helper: test windows                                      */
/* ------------------------------------------------------------------------ */

static void
perf_call_done_cb (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = NULL;
  Runner *r;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  r = the_runner;
  if (r == NULL || r->L == NULL)
    return;

  if (reply == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        runner_fail (r, "The call to gnome-shell-perf-helper failed: %s", error->message);
      return;
    }

  wake_if_parked (GPOINTER_TO_UINT (user_data));
}

static void
send_pending_call (Runner *r)
{
  g_autofree char *method = g_steal_pointer (&r->pending_method);
  GVariant *params = g_steal_pointer (&r->pending_params);

  g_dbus_connection_call (r->bus, PERF_HELPER_NAME, PERF_HELPER_PATH, PERF_HELPER_NAME,
                          method, params, NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                          r->cancellable, perf_call_done_cb,
                          GUINT_TO_POINTER (r->pending_serial));
  g_variant_unref (params);
}

static void
helper_name_appeared_cb (GDBusConnection *connection,
                         const char      *name,
                         const char      *name_owner,
                         gpointer         user_data)
{
  Runner *r = the_runner;

  if (r == NULL || r->helper_ready)
    return;

  r->helper_ready = TRUE;
  g_set_object (&r->bus, connection);
  if (r->pending_method != NULL)
    send_pending_call (r);
}

static void
helper_name_vanished_cb (GDBusConnection *connection,
                         const char      *name,
                         gpointer         user_data)
{
  /* The helper is not up yet, or it has exited as it was asked to */
}

static gboolean
ensure_helper_started (Runner  *r,
                       GError **error)
{
  const char *directory;
  g_autofree char *command = NULL;

  if (r->helper != NULL)
    return TRUE;

  directory = g_getenv ("GNOME_SHELL_BUILDDIR");
  if (directory == NULL || directory[0] == '\0')
    directory = GNOME_SHELL_LIBEXECDIR;

  command = g_build_filename (directory, "gnome-shell-perf-helper", NULL);
  r->helper = g_subprocess_new (G_SUBPROCESS_FLAGS_NONE, error, command, NULL);
  if (r->helper == NULL)
    return FALSE;

  r->helper_auto_exit = TRUE;
  r->helper_watch_id = g_bus_watch_name (G_BUS_TYPE_SESSION, PERF_HELPER_NAME,
                                         G_BUS_NAME_WATCHER_FLAGS_NONE,
                                         helper_name_appeared_cb,
                                         helper_name_vanished_cb,
                                         NULL, NULL);
  return TRUE;
}

/* Sends @method to the helper once it is up, and resumes the script on the
 * reply. @params is floating and consumed. */
static int
perf_helper_call (lua_State  *L,
                  const char *what,
                  const char *method,
                  GVariant   *params)
{
  Runner *r = the_runner;
  g_autoptr (GError) error = NULL;
  guint serial;

  g_variant_ref_sink (params);
  if (L != r->co)
    {
      g_variant_unref (params);
      return luaL_error (L, "test.%s () can only be called by the script itself, not by a callback",
                         what);
    }

  if (!ensure_helper_started (r, &error))
    {
      g_variant_unref (params);
      lua_pushfstring (L, "Could not start gnome-shell-perf-helper: %s", error->message);
      g_clear_error (&error);
      return lua_error (L);
    }

  serial = begin_park (L, what);
  g_free (r->pending_method);
  r->pending_method = g_strdup (method);
  g_clear_pointer (&r->pending_params, g_variant_unref);
  r->pending_params = params;
  r->pending_serial = serial;

  if (r->helper_ready)
    send_pending_call (r);

  return lua_yield (L, 0);
}

static lua_Integer
opt_field_integer (lua_State  *L,
                   int         table,
                   const char *name,
                   lua_Integer def)
{
  lua_Integer value = def;

  if (lua_getfield (L, table, name) != LUA_TNIL)
    {
      int isnum;

      value = lua_tointegerx (L, -1, &isnum);
      if (!isnum)
        luaL_error (L, "field '%s' must be an integer", name);
    }
  lua_pop (L, 1);
  return value;
}

static gboolean
opt_field_boolean (lua_State  *L,
                   int         table,
                   const char *name,
                   gboolean    def)
{
  gboolean value = def;

  if (lua_getfield (L, table, name) != LUA_TNIL)
    {
      if (!lua_isboolean (L, -1))
        luaL_error (L, "field '%s' must be a boolean", name);
      value = lua_toboolean (L, -1);
    }
  lua_pop (L, 1);
  return value;
}

/* test.create_window {width = 640, height = 480, alpha = false,
 * maximized = false, redraws = false, text_input = false} */
static int
l_create_window (lua_State *L)
{
  static const char * const fields[] = {
    "width", "height", "alpha", "maximized", "redraws", "text_input", NULL
  };
  lua_Integer width, height;
  gboolean alpha, maximized, redraws, text_input;

  if (lua_isnoneornil (L, 1))
    lua_newtable (L);
  luaL_checktype (L, 1, LUA_TTABLE);

  /* Params.parse () did not allow unknown parameters */
  lua_pushnil (L);
  while (lua_next (L, 1) != 0)
    {
      const char *key = lua_type (L, -2) == LUA_TSTRING ? lua_tostring (L, -2) : NULL;

      if (key == NULL || !g_strv_contains (fields, key))
        return luaL_error (L, "unknown window parameter '%s'",
                           key != NULL ? key : luaL_typename (L, -2));
      lua_pop (L, 1);
    }

  width = opt_field_integer (L, 1, "width", 640);
  height = opt_field_integer (L, 1, "height", 480);
  alpha = opt_field_boolean (L, 1, "alpha", FALSE);
  maximized = opt_field_boolean (L, 1, "maximized", FALSE);
  redraws = opt_field_boolean (L, 1, "redraws", FALSE);
  text_input = opt_field_boolean (L, 1, "text_input", FALSE);

  return perf_helper_call (L, "create_window", "CreateWindow",
                           g_variant_new ("(iibbbb)", (int) width, (int) height,
                                          alpha, maximized, redraws, text_input));
}

static int
l_wait_windows (lua_State *L)
{
  return perf_helper_call (L, "wait_windows", "WaitWindows", g_variant_new ("()"));
}

static int
l_destroy_windows (lua_State *L)
{
  return perf_helper_call (L, "destroy_windows", "DestroyWindows", g_variant_new ("()"));
}

/* Don't exit the perf helper after running the script: it then remains
 * until something else makes it exit, e.g. the Wayland socket closing */
static int
l_disable_helper_auto_exit (lua_State *L)
{
  Runner *r = the_runner;
  g_autoptr (GError) error = NULL;

  if (!ensure_helper_started (r, &error))
    {
      lua_pushfstring (L, "Could not start gnome-shell-perf-helper: %s", error->message);
      g_clear_error (&error);
      return lua_error (L);
    }

  r->helper_auto_exit = FALSE;
  return 0;
}

static void
helper_exit_done_cb (GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = NULL;
  Runner *r;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  r = the_runner;
  if (r == NULL || r->L == NULL)
    return;

  if (reply == NULL && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    runner_fail (r, "Failed to exit gnome-shell-perf-helper: %s", error->message);
  else
    runner_finish (r, NULL);
}

/* ------------------------------------------------------------------------ */
/* Objects the script creates                                                 */
/* ------------------------------------------------------------------------ */

typedef struct
{
  lua_State *L;
  GType type;
  GObjectClass *klass;
  GArray *pspecs;   /* GParamSpec * */
  GArray *values;   /* GValue */
} NewObject;

static int
convert_properties_cb (lua_State *L)
{
  NewObject *info = lua_touserdata (L, 1);

  luaL_checkstack (L, 6, "properties");
  lua_pushnil (L);
  while (lua_next (L, 2) != 0)
    {
      g_autofree char *name = NULL;
      GParamSpec *pspec;
      GValue value = G_VALUE_INIT;

      if (lua_type (L, -2) != LUA_TSTRING)
        return luaL_error (L, "property names must be strings");

      name = g_strdelimit (g_strdup (lua_tostring (L, -2)), "_", '-');
      pspec = g_object_class_find_property (info->klass, name);
      if (pspec == NULL)
        return luaL_error (L, "%s has no property '%s'", g_type_name (info->type), name);

      g_value_init (&value, G_PARAM_SPEC_VALUE_TYPE (pspec));
      g_array_append_val (info->values, value);
      g_array_append_val (info->pspecs, pspec);
      shell_lua_to_gvalue (L, -1, &g_array_index (info->values, GValue,
                                                  info->values->len - 1));
      lua_pop (L, 1);
    }

  return 0;
}

/* test.new (type_name, {property = value ...}) -> the object */
static int
l_new (lua_State *L)
{
  Runner *r = the_runner;
  const char *type_name = luaL_checkstring (L, 1);
  NewObject info = { L, 0, NULL, NULL, NULL };
  GObject *object = NULL;
  const char **names = NULL;
  GValue *values = NULL;
  guint i, n;
  int status;

  if (lua_isnoneornil (L, 2))
    lua_newtable (L);
  luaL_checktype (L, 2, LUA_TTABLE);

  info.type = g_type_from_name (type_name);
  if (info.type == 0 || !g_type_is_a (info.type, G_TYPE_OBJECT) ||
      G_TYPE_IS_ABSTRACT (info.type))
    return luaL_error (L, "'%s' is not the name of a type of objects that can be created", type_name);

  info.klass = g_type_class_ref (info.type);
  info.pspecs = g_array_new (FALSE, FALSE, sizeof (GParamSpec *));
  info.values = g_array_new (FALSE, TRUE, sizeof (GValue));
  g_array_set_clear_func (info.values, (GDestroyNotify) g_value_unset);

  lua_pushcfunction (L, convert_properties_cb);
  lua_pushlightuserdata (L, &info);
  lua_pushvalue (L, 2);
  status = lua_pcall (L, 2, 0, 0);

  if (status == LUA_OK)
    {
      n = info.pspecs->len;
      names = g_new0 (const char *, n + 1);
      values = g_new0 (GValue, n + 1);
      for (i = 0; i < n; i++)
        {
          names[i] = g_param_spec_get_name (g_array_index (info.pspecs, GParamSpec *, i));
          values[i] = g_array_index (info.values, GValue, i);
        }
      object = g_object_new_with_properties (info.type, n, names, values);
      g_free (names);
      g_free (values); /* shallow copies: the array owns the contents */
    }

  g_array_unref (info.pspecs);
  g_array_unref (info.values);
  g_type_class_unref (info.klass);

  if (status != LUA_OK)
    return lua_error (L);

  keep_object (r, object);
  g_object_unref (object);
  shell_lua_inspector_push_object (L, object);
  return 1;
}

/* test.set_boolean (schema_id, key, value) */
static int
l_set_boolean (lua_State *L)
{
  const char *schema_id = luaL_checkstring (L, 1);
  const char *key = luaL_checkstring (L, 2);
  gboolean value;
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  GSettingsSchema *schema;
  GSettings *settings;

  luaL_checktype (L, 3, LUA_TBOOLEAN);
  value = lua_toboolean (L, 3);

  schema = g_settings_schema_source_lookup (source, schema_id, TRUE);
  if (schema == NULL)
    return luaL_error (L, "no such schema: %s", schema_id);
  if (!g_settings_schema_has_key (schema, key))
    {
      g_settings_schema_unref (schema);
      return luaL_error (L, "schema %s has no key '%s'", schema_id, key);
    }

  settings = g_settings_new_full (schema, NULL, NULL);
  g_settings_schema_unref (schema);
  g_settings_set_boolean (settings, key, value);
  g_settings_sync ();
  g_object_unref (settings);
  return 0;
}

/* test.create_virtual_device (type): a virtual input device of the default
 * seat, for the type with the nick of ClutterInputDeviceType
 * (`keyboard-device`); it stays until the shell exits */
static int
l_create_virtual_device (lua_State *L)
{
  Runner *r = the_runner;
  ClutterInputDeviceType type = check_enum (L, 1, CLUTTER_TYPE_INPUT_DEVICE_TYPE);
  ClutterStage *stage = shell_global_get_stage (shell_global_get ());
  ClutterContext *context = clutter_actor_get_context (CLUTTER_ACTOR (stage));
  ClutterSeat *seat = clutter_backend_get_default_seat (clutter_context_get_backend (context));
  ClutterVirtualInputDevice *device = clutter_seat_create_virtual_device (seat, type);

  g_ptr_array_add (r->keep, device);
  return 0;
}

/* test.press_accelerator (accelerator): types the combination (GTK syntax:
 * `<Super><Alt>j`, `<Control>F5`; modifiers Shift, Control, Alt, Super; a key
 * is one ASCII character, `space`, `Return`, `Escape`, `Tab`, `Print` or
 * `F1`..`F12`) on a virtual keyboard of the default seat: the modifiers go
 * down, the key goes down and up, the modifiers go up. It does not wait: the
 * events reach the shell from the main loop, so the script polls for the
 * effect. The keyboard is made on the first call. */
static ClutterVirtualInputDevice *accelerator_keyboard = NULL;

static gboolean
accelerator_keyval (const char *name,
                    guint      *keyval)
{
  static const struct
  {
    const char *name;
    guint keyval;
  } named[] = {
    { "space", CLUTTER_KEY_space },
    { "Return", CLUTTER_KEY_Return },
    { "Escape", CLUTTER_KEY_Escape },
    { "Tab", CLUTTER_KEY_Tab },
    { "Print", CLUTTER_KEY_Print },
  };
  guint i;

  if (name[0] != '\0' && name[1] == '\0' && (guchar) name[0] > ' ' && (guchar) name[0] < 0x7f)
    {
      *keyval = (guchar) name[0];
      return TRUE;
    }

  if (name[0] == 'F' && name[1] >= '1' && name[1] <= '9' &&
      (name[2] == '\0' || (name[2] >= '0' && name[2] <= '2' && name[3] == '\0')))
    {
      int n = atoi (name + 1);

      if (n >= 1 && n <= 12)
        {
          *keyval = CLUTTER_KEY_F1 + n - 1;
          return TRUE;
        }
    }

  for (i = 0; i < G_N_ELEMENTS (named); i++)
    {
      if (strcmp (name, named[i].name) == 0)
        {
          *keyval = named[i].keyval;
          return TRUE;
        }
    }

  return FALSE;
}

static int
l_press_accelerator (lua_State *L)
{
  Runner *r = the_runner;
  const char *accelerator = luaL_checkstring (L, 1);
  guint modifiers[8];
  guint n_modifiers = 0, keyval = 0, i;
  const char *p = accelerator;
  int64_t time_us;

  while (*p == '<')
    {
      const char *end = strchr (p, '>');
      g_autofree char *modifier = end != NULL ? g_ascii_strdown (p + 1, end - p - 1) : NULL;
      guint modifier_keyval;

      if (modifier == NULL || n_modifiers == G_N_ELEMENTS (modifiers))
        return luaL_error (L, "'%s' is not an accelerator", accelerator);

      if (strcmp (modifier, "super") == 0)
        modifier_keyval = CLUTTER_KEY_Super_L;
      else if (strcmp (modifier, "control") == 0 || strcmp (modifier, "ctrl") == 0)
        modifier_keyval = CLUTTER_KEY_Control_L;
      else if (strcmp (modifier, "alt") == 0)
        modifier_keyval = CLUTTER_KEY_Alt_L;
      else if (strcmp (modifier, "shift") == 0)
        modifier_keyval = CLUTTER_KEY_Shift_L;
      else
        return luaL_error (L, "'%s': unsupported modifier <%s>", accelerator, modifier);

      modifiers[n_modifiers++] = modifier_keyval;
      p = end + 1;
    }

  if (!accelerator_keyval (p, &keyval))
    return luaL_error (L, "'%s': unsupported key '%s'", accelerator, p);

  if (accelerator_keyboard == NULL)
    {
      ClutterStage *stage = shell_global_get_stage (shell_global_get ());
      ClutterContext *context = clutter_actor_get_context (CLUTTER_ACTOR (stage));
      ClutterSeat *seat = clutter_backend_get_default_seat (clutter_context_get_backend (context));
      ClutterVirtualInputDevice *device =
        clutter_seat_create_virtual_device (seat, CLUTTER_KEYBOARD_DEVICE);

      /* r->keep releases it when the runner goes */
      g_ptr_array_add (r->keep, device);
      g_set_weak_pointer (&accelerator_keyboard, device);
    }

  time_us = g_get_monotonic_time ();
  for (i = 0; i < n_modifiers; i++)
    clutter_virtual_input_device_notify_keyval (accelerator_keyboard, time_us++,
                                                modifiers[i], CLUTTER_KEY_STATE_PRESSED);
  clutter_virtual_input_device_notify_keyval (accelerator_keyboard, time_us++,
                                              keyval, CLUTTER_KEY_STATE_PRESSED);
  clutter_virtual_input_device_notify_keyval (accelerator_keyboard, time_us++,
                                              keyval, CLUTTER_KEY_STATE_RELEASED);
  for (i = n_modifiers; i > 0; i--)
    clutter_virtual_input_device_notify_keyval (accelerator_keyboard, time_us++,
                                                modifiers[i - 1], CLUTTER_KEY_STATE_RELEASED);

  return 0;
}

/* Virtual monitors: Mutter's test library is loaded when the first is made,
 * it is not a dependency of the shell */

static gboolean
load_test_library (Runner  *r,
                   GError **error)
{
  const char *name = g_getenv (MUTTER_TEST_LIBRARY_ENV);

  if (r->test_library != NULL)
    return TRUE;

  if (name == NULL || name[0] == '\0')
    name = MUTTER_TEST_LIBRARY;

  r->test_library = dlopen (name, RTLD_NOW | RTLD_GLOBAL);
  if (r->test_library == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not load %s: %s", name, dlerror ());
      return FALSE;
    }

  r->monitor_new = (TestMonitorNewFunc) dlsym (r->test_library, "meta_test_monitor_new");
  r->monitor_destroy = (TestMonitorDestroyFunc) dlsym (r->test_library, "meta_test_monitor_destroy");
  if (r->monitor_new == NULL || r->monitor_destroy == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "%s has no virtual monitors (meta_test_monitor_new ())", name);
      dlclose (r->test_library);
      r->test_library = NULL;
      return FALSE;
    }

  return TRUE;
}

/* test.new_monitor (width, height, refresh_rate) -> monitor, which has
 * :destroy () */
static int
l_new_monitor (lua_State *L)
{
  Runner *r = the_runner;
  lua_Integer width = luaL_checkinteger (L, 1);
  lua_Integer height = luaL_checkinteger (L, 2);
  lua_Number refresh_rate = luaL_checknumber (L, 3);
  g_autoptr (GError) error = NULL;
  TestMonitor *monitor, **udata;
  GObject *object;

  if (!load_test_library (r, &error))
    {
      lua_pushstring (L, error->message);
      g_clear_error (&error);
      return lua_error (L);
    }

  object = r->monitor_new (shell_global_get_context (shell_global_get ()),
                           (int) width, (int) height, (float) refresh_rate, &error);
  if (object == NULL)
    {
      lua_pushfstring (L, "Could not create a virtual monitor: %s", error->message);
      g_clear_error (&error);
      return lua_error (L);
    }

  monitor = g_new0 (TestMonitor, 1);
  monitor->monitor = object;
  g_ptr_array_add (r->monitors, monitor);

  udata = lua_newuserdatauv (L, sizeof (TestMonitor *), 0);
  *udata = monitor;
  luaL_setmetatable (L, MONITOR_METATABLE);
  return 1;
}

static void
destroy_monitor (Runner      *r,
                 TestMonitor *monitor)
{
  GObject *object = g_steal_pointer (&monitor->monitor);

  /* meta_test_monitor_destroy () takes the reference of the creation */
  if (object != NULL)
    r->monitor_destroy (object);
}

static int
l_monitor_destroy (lua_State *L)
{
  TestMonitor *monitor = *(TestMonitor **) luaL_checkudata (L, 1, MONITOR_METATABLE);

  destroy_monitor (the_runner, monitor);
  return 0;
}

/* ------------------------------------------------------------------------ */
/* Methods on the handles of the objects of the shell                         */
/* ------------------------------------------------------------------------ */

/* ClutterActor */

static int
l_actor_get_transformed_position (lua_State *L)
{
  ClutterActor *actor = check_instance (L, 1, CLUTTER_TYPE_ACTOR);
  float x, y;

  clutter_actor_get_transformed_position (actor, &x, &y);
  lua_pushnumber (L, x);
  lua_pushnumber (L, y);
  return 2;
}

/* actor:get_allocation_box () -> {x, y, width, height} */
static int
l_actor_get_allocation_box (lua_State *L)
{
  ClutterActor *actor = check_instance (L, 1, CLUTTER_TYPE_ACTOR);
  ClutterActorBox box;

  clutter_actor_get_allocation_box (actor, &box);
  push_rectangle (L, box.x1, box.y1, clutter_actor_box_get_width (&box),
                  clutter_actor_box_get_height (&box));
  return 1;
}

static int
l_actor_queue_redraw (lua_State *L)
{
  clutter_actor_queue_redraw (check_instance (L, 1, CLUTTER_TYPE_ACTOR));
  return 0;
}

static const luaL_Reg actor_methods[] = {
  { "get_transformed_position", l_actor_get_transformed_position },
  { "get_allocation_box", l_actor_get_allocation_box },
  { "queue_redraw", l_actor_queue_redraw },
  { NULL, NULL }
};

/* stage:get_actor_at_pos (pick_mode, x, y) */
static int
l_stage_get_actor_at_pos (lua_State *L)
{
  ClutterStage *stage = check_instance (L, 1, CLUTTER_TYPE_STAGE);
  ClutterPickMode mode = check_enum (L, 2, CLUTTER_TYPE_PICK_MODE);
  lua_Number x = luaL_checknumber (L, 3);
  lua_Number y = luaL_checknumber (L, 4);

  push_object_or_nil (L, clutter_stage_get_actor_at_pos (stage, mode, x, y));
  return 1;
}

static const luaL_Reg stage_methods[] = {
  { "get_actor_at_pos", l_stage_get_actor_at_pos },
  { NULL, NULL }
};

/* widget:navigate_focus (from, direction, wrap_around) -> boolean */
static int
l_widget_navigate_focus (lua_State *L)
{
  StWidget *widget = check_instance (L, 1, ST_TYPE_WIDGET);
  ClutterActor *from = opt_instance (L, 2, CLUTTER_TYPE_ACTOR);
  StDirectionType direction = check_enum (L, 3, ST_TYPE_DIRECTION_TYPE);
  gboolean wrap_around;

  luaL_checktype (L, 4, LUA_TBOOLEAN);
  wrap_around = lua_toboolean (L, 4);

  lua_pushboolean (L, st_widget_navigate_focus (widget, from, direction, wrap_around));
  return 1;
}

static const luaL_Reg widget_methods[] = {
  { "navigate_focus", l_widget_navigate_focus },
  { NULL, NULL }
};

/* ShellOverview */

static int
l_overview_show (lua_State *L)
{
  shell_overview_show (check_instance (L, 1, SHELL_TYPE_OVERVIEW));
  return 0;
}

static int
l_overview_hide (lua_State *L)
{
  shell_overview_hide (check_instance (L, 1, SHELL_TYPE_OVERVIEW));
  return 0;
}

/* overview:show_for_reason (reason [, state]): state is "window-picker"
 * (default) or "app-grid" */
static int
l_overview_show_for_reason (lua_State *L)
{
  ShellOverview *overview = check_instance (L, 1, SHELL_TYPE_OVERVIEW);
  const char *reason = luaL_checkstring (L, 2);
  const char *state = luaL_optstring (L, 3, "window-picker");
  ShellControlsState controls_state;

  if (g_str_equal (state, "window-picker"))
    controls_state = SHELL_CONTROLS_STATE_WINDOW_PICKER;
  else if (g_str_equal (state, "app-grid"))
    controls_state = SHELL_CONTROLS_STATE_APP_GRID;
  else
    return luaL_argerror (L, 3, "expected 'window-picker' or 'app-grid'");

  shell_overview_show_for_reason (overview, controls_state, reason);
  return 0;
}

/* overview:hide_for_reason (reason) */
static int
l_overview_hide_for_reason (lua_State *L)
{
  ShellOverview *overview = check_instance (L, 1, SHELL_TYPE_OVERVIEW);

  shell_overview_hide_for_reason (overview, luaL_checkstring (L, 2));
  return 0;
}

static const luaL_Reg overview_methods[] = {
  { "show", l_overview_show },
  { "hide", l_overview_hide },
  { "show_for_reason", l_overview_show_for_reason },
  { "hide_for_reason", l_overview_hide_for_reason },
  { NULL, NULL }
};

/* ShellSearchController and ShellSearchResult (tests/api/search.lua) */

/* controller:search (text): types @text in the search entry, as the user
 * would; the search starts after the debounce of the results view */
static int
l_search_controller_search (lua_State *L)
{
  ShellSearchController *controller = check_instance (L, 1, SHELL_TYPE_SEARCH_CONTROLLER);
  StEntry *entry = NULL;

  g_object_get (controller, "search-entry", &entry, NULL);
  if (entry == NULL)
    return luaL_error (L, "the search controller has no entry");

  st_entry_set_text (entry, luaL_checkstring (L, 2));
  g_object_unref (entry);
  return 0;
}

/* controller:search_in_progress () -> boolean: a search is starting or a
 * provider has not answered yet */
static int
l_search_controller_search_in_progress (lua_State *L)
{
  ShellSearchController *controller = check_instance (L, 1, SHELL_TYPE_SEARCH_CONTROLLER);
  ShellSearchResultsView *view = shell_search_controller_get_search_results (controller);

  lua_pushboolean (L, view != NULL && shell_search_results_view_get_search_in_progress (view));
  return 1;
}

/* controller:n_results (provider_id) -> the number of results the section of
 * the provider shows */
static int
l_search_controller_n_results (lua_State *L)
{
  ShellSearchController *controller = check_instance (L, 1, SHELL_TYPE_SEARCH_CONTROLLER);
  ShellSearchResultsView *view = shell_search_controller_get_search_results (controller);

  lua_pushinteger (L, view != NULL
                        ? shell_search_results_view_get_n_results (view, luaL_checkstring (L, 2))
                        : 0);
  return 1;
}

/* controller:get_result (provider_id, index) -> the result actor (1-based) or nil */
static int
l_search_controller_get_result (lua_State *L)
{
  ShellSearchController *controller = check_instance (L, 1, SHELL_TYPE_SEARCH_CONTROLLER);
  ShellSearchResultsView *view = shell_search_controller_get_search_results (controller);
  lua_Integer index = luaL_checkinteger (L, 3);

  push_object_or_nil (L, view != NULL && index >= 1
                           ? shell_search_results_view_get_result (view, luaL_checkstring (L, 2),
                                                                   (guint) (index - 1))
                           : NULL);
  return 1;
}

static const luaL_Reg search_controller_methods[] = {
  { "search", l_search_controller_search },
  { "search_in_progress", l_search_controller_search_in_progress },
  { "n_results", l_search_controller_n_results },
  { "get_result", l_search_controller_get_result },
  { NULL, NULL }
};

/* result:activate (): activates the result as a click does */
static int
l_search_result_activate (lua_State *L)
{
  shell_search_result_activate (check_instance (L, 1, SHELL_TYPE_SEARCH_RESULT));
  return 0;
}

/* result:launch_search (): what a click on the header of the section of the
 * provider of the result does (the "launch_search" of the provider, with the
 * terms of the search) */
static int
l_search_result_launch_search (lua_State *L)
{
  ShellSearchResult *result = check_instance (L, 1, SHELL_TYPE_SEARCH_RESULT);
  g_autoptr (ShellSearchResultsView) view = shell_search_result_get_results_view (result);
  g_auto (GStrv) terms = view != NULL ? shell_search_results_view_get_terms (view) : NULL;

  if (view == NULL)
    return luaL_error (L, "the result is not shown in a results view");

  shell_search_provider_launch_search (shell_search_result_get_provider (result),
                                       (const char * const *) terms);
  return 0;
}

static const luaL_Reg search_result_methods[] = {
  { "activate", l_search_result_activate },
  { "launch_search", l_search_result_launch_search },
  { NULL, NULL }
};

/* ShellPanel */

static int
l_panel_get_status_area_indicator (lua_State *L)
{
  ShellPanel *panel = check_instance (L, 1, SHELL_TYPE_PANEL);
  const char *role = luaL_checkstring (L, 2);

  push_object_or_nil (L, shell_panel_get_status_area_indicator (panel, role));
  return 1;
}

static const luaL_Reg panel_methods[] = {
  { "get_status_area_indicator", l_panel_get_status_area_indicator },
  { NULL, NULL }
};

/* ShellPopupMenuBase: open and close with the animation of the JS default */

static int
l_menu_open (lua_State *L)
{
  ShellPopupMenuBase *menu = check_instance (L, 1, SHELL_TYPE_POPUP_MENU_BASE);

  lua_pushboolean (L, shell_popup_menu_base_open (menu, SHELL_POPUP_ANIMATION_FULL));
  return 1;
}

static int
l_menu_close (lua_State *L)
{
  ShellPopupMenuBase *menu = check_instance (L, 1, SHELL_TYPE_POPUP_MENU_BASE);

  lua_pushboolean (L, shell_popup_menu_base_close (menu, SHELL_POPUP_ANIMATION_FULL));
  return 1;
}

static const luaL_Reg menu_methods[] = {
  { "open", l_menu_open },
  { "close", l_menu_close },
  { NULL, NULL }
};

/* ShellMessageTray, ShellSource, ShellNotification */

static int
l_tray_get_system_source (lua_State *L)
{
  ShellMessageTray *tray = check_instance (L, 1, SHELL_TYPE_MESSAGE_TRAY);

  push_object_or_nil (L, shell_message_tray_get_system_source (tray));
  return 1;
}

static const luaL_Reg tray_methods[] = {
  { "get_system_source", l_tray_get_system_source },
  { NULL, NULL }
};

static int
l_source_add_notification (lua_State *L)
{
  ShellSource *source = check_instance (L, 1, SHELL_TYPE_SOURCE);
  ShellNotification *notification = check_instance (L, 2, SHELL_TYPE_NOTIFICATION);

  shell_source_add_notification (source, notification);
  return 0;
}

static const luaL_Reg source_methods[] = {
  { "add_notification", l_source_add_notification },
  { NULL, NULL }
};

/* notification:destroy ([reason]), the reason is dismissed by default */
static int
l_notification_destroy (lua_State *L)
{
  ShellNotification *notification = check_instance (L, 1, SHELL_TYPE_NOTIFICATION);
  ShellNotificationDestroyedReason reason = SHELL_NOTIFICATION_DESTROYED_REASON_DISMISSED;

  if (!lua_isnoneornil (L, 2))
    reason = check_enum (L, 2, SHELL_TYPE_NOTIFICATION_DESTROYED_REASON);

  shell_notification_destroy (notification, reason);
  return 0;
}

static const luaL_Reg notification_methods[] = {
  { "destroy", l_notification_destroy },
  { NULL, NULL }
};

/* ShellLayoutManager */

static int
l_layout_get_monitor (lua_State *L)
{
  ShellLayoutManager *layout_manager = check_instance (L, 1, SHELL_TYPE_LAYOUT_MANAGER);
  int index = luaL_checkinteger (L, 2);
  ShellMonitor *monitor = shell_layout_manager_get_monitor (layout_manager, index);

  push_monitor (L, monitor);
  g_clear_pointer (&monitor, shell_monitor_free);
  return 1;
}

static int
l_layout_get_primary_monitor (lua_State *L)
{
  ShellLayoutManager *layout_manager = check_instance (L, 1, SHELL_TYPE_LAYOUT_MANAGER);
  ShellMonitor *monitor = shell_layout_manager_get_primary_monitor (layout_manager);

  push_monitor (L, monitor);
  g_clear_pointer (&monitor, shell_monitor_free);
  return 1;
}

static int
l_layout_get_n_background_managers (lua_State *L)
{
  ShellLayoutManager *layout_manager = check_instance (L, 1, SHELL_TYPE_LAYOUT_MANAGER);

  lua_pushinteger (L, shell_layout_manager_get_n_background_managers (layout_manager));
  return 1;
}

static const luaL_Reg layout_methods[] = {
  { "get_monitor", l_layout_get_monitor },
  { "get_primary_monitor", l_layout_get_primary_monitor },
  { "get_n_background_managers", l_layout_get_n_background_managers },
  { NULL, NULL }
};

/* MetaDisplay, MetaBackend, MetaWindow */

static int
l_display_get_n_monitors (lua_State *L)
{
  MetaDisplay *display = check_instance (L, 1, META_TYPE_DISPLAY);

  lua_pushinteger (L, meta_display_get_n_monitors (display));
  return 1;
}

static int
l_display_get_monitor_geometry (lua_State *L)
{
  MetaDisplay *display = check_instance (L, 1, META_TYPE_DISPLAY);
  int index = luaL_checkinteger (L, 2);
  MtkRectangle geometry;

  if (index < 0 || index >= meta_display_get_n_monitors (display))
    return luaL_error (L, "no monitor %d", index);

  meta_display_get_monitor_geometry (display, index, &geometry);
  push_rectangle (L, geometry.x, geometry.y, geometry.width, geometry.height);
  return 1;
}

static const luaL_Reg display_methods[] = {
  { "get_n_monitors", l_display_get_n_monitors },
  { "get_monitor_geometry", l_display_get_monitor_geometry },
  { NULL, NULL }
};

static int
l_backend_get_monitor_manager (lua_State *L)
{
  MetaBackend *backend = check_instance (L, 1, META_TYPE_BACKEND);

  push_object_or_nil (L, meta_backend_get_monitor_manager (backend));
  return 1;
}

static const luaL_Reg backend_methods[] = {
  { "get_monitor_manager", l_backend_get_monitor_manager },
  { NULL, NULL }
};

static int
l_window_get_compositor_private (lua_State *L)
{
  MetaWindow *window = check_instance (L, 1, META_TYPE_WINDOW);

  push_object_or_nil (L, meta_window_get_compositor_private (window));
  return 1;
}

/* window:delete (timestamp) */
static int
l_window_delete (lua_State *L)
{
  MetaWindow *window = check_instance (L, 1, META_TYPE_WINDOW);
  lua_Integer timestamp = luaL_checkinteger (L, 2);

  meta_window_delete (window, (guint32) timestamp);
  return 0;
}

static const luaL_Reg window_methods[] = {
  { "get_compositor_private", l_window_get_compositor_private },
  { "delete", l_window_delete },
  { NULL, NULL }
};

/* ShellGlobal, ShellAppSystem, ShellApp */

static int
l_global_get_current_time (lua_State *L)
{
  ShellGlobal *global = check_instance (L, 1, SHELL_TYPE_GLOBAL);

  lua_pushinteger (L, shell_global_get_current_time (global));
  return 1;
}

static int
l_global_get_app_system (lua_State *L)
{
  ShellGlobal *global = check_instance (L, 1, SHELL_TYPE_GLOBAL);

  push_object_or_nil (L, shell_global_get_app_system (global));
  return 1;
}

static const luaL_Reg global_methods[] = {
  { "get_current_time", l_global_get_current_time },
  { "get_app_system", l_global_get_app_system },
  { NULL, NULL }
};

static int
l_app_system_lookup_app (lua_State *L)
{
  ShellAppSystem *app_system = check_instance (L, 1, SHELL_TYPE_APP_SYSTEM);
  const char *id = luaL_checkstring (L, 2);

  push_object_or_nil (L, shell_app_system_lookup_app (app_system, id));
  return 1;
}

static const luaL_Reg app_system_methods[] = {
  { "lookup_app", l_app_system_lookup_app },
  { NULL, NULL }
};

static int
l_app_activate (lua_State *L)
{
  shell_app_activate (check_instance (L, 1, SHELL_TYPE_APP));
  return 0;
}

/* app:get_windows () -> array of MetaWindow */
static int
l_app_get_windows (lua_State *L)
{
  ShellApp *app = check_instance (L, 1, SHELL_TYPE_APP);
  GSList *windows = shell_app_get_windows (app);
  GSList *l;
  int i = 1;

  lua_newtable (L);
  for (l = windows; l != NULL; l = l->next)
    {
      luaL_checkstack (L, 4, "get_windows");
      shell_lua_inspector_push_object (L, G_OBJECT (l->data));
      lua_rawseti (L, -2, i++);
    }
  return 1;
}

static const luaL_Reg app_methods[] = {
  { "activate", l_app_activate },
  { "get_windows", l_app_get_windows },
  { NULL, NULL }
};

static void
register_object_methods (void)
{
  static gboolean registered = FALSE;

  if (registered)
    return;
  registered = TRUE;

  shell_lua_inspector_add_object_methods (G_TYPE_OBJECT, object_methods);
  shell_lua_inspector_add_object_methods (CLUTTER_TYPE_ACTOR, actor_methods);
  shell_lua_inspector_add_object_methods (CLUTTER_TYPE_STAGE, stage_methods);
  shell_lua_inspector_add_object_methods (ST_TYPE_WIDGET, widget_methods);
  shell_lua_inspector_add_object_methods (SHELL_TYPE_OVERVIEW, overview_methods);
  shell_lua_inspector_add_object_methods (SHELL_TYPE_SEARCH_CONTROLLER, search_controller_methods);
  shell_lua_inspector_add_object_methods (SHELL_TYPE_SEARCH_RESULT, search_result_methods);
  shell_lua_inspector_add_object_methods (SHELL_TYPE_PANEL, panel_methods);
  shell_lua_inspector_add_object_methods (SHELL_TYPE_POPUP_MENU_BASE, menu_methods);
  shell_lua_inspector_add_object_methods (SHELL_TYPE_MESSAGE_TRAY, tray_methods);
  shell_lua_inspector_add_object_methods (SHELL_TYPE_SOURCE, source_methods);
  shell_lua_inspector_add_object_methods (SHELL_TYPE_NOTIFICATION, notification_methods);
  shell_lua_inspector_add_object_methods (SHELL_TYPE_LAYOUT_MANAGER, layout_methods);
  shell_lua_inspector_add_object_methods (META_TYPE_DISPLAY, display_methods);
  shell_lua_inspector_add_object_methods (META_TYPE_BACKEND, backend_methods);
  shell_lua_inspector_add_object_methods (META_TYPE_WINDOW, window_methods);
  shell_lua_inspector_add_object_methods (SHELL_TYPE_GLOBAL, global_methods);
  shell_lua_inspector_add_object_methods (SHELL_TYPE_APP_SYSTEM, app_system_methods);
  shell_lua_inspector_add_object_methods (SHELL_TYPE_APP, app_methods);
}

/* ------------------------------------------------------------------------ */
/* Tests of the extension API (tests/api)                                     */
/* ------------------------------------------------------------------------ */

#define STEP_HOOK "test.step"

typedef struct
{
  char     *name;
  gboolean  ok;
  char     *message;
} Check;

typedef struct
{
  GPtrArray  *checks;  /* Check */
  GHashTable *done;    /* label (owned) -> label, a set */
} ExtensionReport;

typedef struct
{
  const char *level;
  char       *message;
} CapturedLog;

static void
check_free (Check *check)
{
  g_free (check->name);
  g_free (check->message);
  g_free (check);
}

static ExtensionReport *
extension_report_new (void)
{
  ExtensionReport *report = g_new0 (ExtensionReport, 1);

  report->checks = g_ptr_array_new_with_free_func ((GDestroyNotify) check_free);
  report->done = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  return report;
}

static void
extension_report_free (ExtensionReport *report)
{
  g_ptr_array_unref (report->checks);
  g_hash_table_unref (report->done);
  g_free (report);
}

static void
captured_log_free (CapturedLog *entry)
{
  g_free (entry->message);
  g_free (entry);
}

/* The report of the extension @uuid, made if there is none */
static ExtensionReport *
get_report (Runner     *r,
            const char *uuid)
{
  ExtensionReport *report = g_hash_table_lookup (r->reports, uuid);

  if (report == NULL)
    {
      report = extension_report_new ();
      g_hash_table_insert (r->reports, g_strdup (uuid), report);
    }

  return report;
}

/* shell.test.report (name, ok [, message]): one check of a fixture extension
 * (sandboxed states only). A failed check is logged at once, so that the log
 * of a run that aborts says what failed. */
static int
l_api_report (lua_State *L)
{
  Runner *r = the_runner;
  ShellLuaState *state = shell_lua_state_from_L (L);
  const char *name = luaL_checkstring (L, 1);
  const char *message = luaL_optstring (L, 3, NULL);
  const char *uuid = state != NULL ? shell_lua_state_get_id (state) : "?";
  Check *check;
  gboolean ok;

  luaL_checktype (L, 2, LUA_TBOOLEAN);
  ok = lua_toboolean (L, 2);
  if (r == NULL)
    return 0;

  check = g_new0 (Check, 1);
  check->name = g_strdup (name);
  check->ok = ok;
  check->message = g_strdup (message);
  g_ptr_array_add (get_report (r, uuid)->checks, check);

  if (!ok)
    g_printerr ("API check failed in %s: %s: %s\n", uuid, name, message != NULL ? message : "");
  return 0;
}

/* shell.test.done ([label]): the fixture finished the main part ("main") or
 * the step @label */
static int
l_api_done (lua_State *L)
{
  Runner *r = the_runner;
  ShellLuaState *state = shell_lua_state_from_L (L);
  const char *label = luaL_optstring (L, 1, "main");
  const char *uuid = state != NULL ? shell_lua_state_get_id (state) : "?";

  if (r != NULL)
    g_hash_table_add (get_report (r, uuid)->done, g_strdup (label));
  return 0;
}

static const luaL_Reg api_test_functions[] = {
  { "report", l_api_report },
  { "done", l_api_done },
  { NULL, NULL }
};

/* The `shell.test` table of the sandbox, registered when the runner starts:
 * the shells that do not run a test script do not have it */
static void
api_test_register (ShellLuaState *state,
                   lua_State     *L)
{
  int shell = lua_absindex (L, -1);

  luaL_checkstack (L, 4, "shell.test");
  lua_newtable (L);
  luaL_setfuncs (L, api_test_functions, 0);
  lua_setfield (L, shell, "test");
}

static ShellExtensionManager *
check_extension_manager (lua_State *L)
{
  ShellExtensionManager *manager = shell_main_get_extension_manager (shell_main_get_default ());

  if (manager == NULL)
    luaL_error (L, "the extension manager is not up");

  return manager;
}

/* test.extension_enable (uuid) */
static int
l_extension_enable (lua_State *L)
{
  Runner *r = the_runner;
  const char *uuid = luaL_checkstring (L, 1);
  ShellExtensionManager *manager = check_extension_manager (L);

  g_hash_table_remove (r->reports, uuid);
  if (!shell_extension_manager_enable_extension (manager, uuid))
    return luaL_error (L, "extension %s is not installed", uuid);

  return 0;
}

/* test.extension_disable (uuid) */
static int
l_extension_disable (lua_State *L)
{
  const char *uuid = luaL_checkstring (L, 1);
  ShellExtensionManager *manager = check_extension_manager (L);

  if (!shell_extension_manager_disable_extension (manager, uuid))
    return luaL_error (L, "extension %s is not installed", uuid);

  return 0;
}

static const char *
state_nick (ShellLuaExtensionState state)
{
  switch (state)
    {
    case SHELL_LUA_EXTENSION_STATE_ACTIVE:
      return "active";
    case SHELL_LUA_EXTENSION_STATE_INACTIVE:
      return "inactive";
    case SHELL_LUA_EXTENSION_STATE_ERROR:
      return "error";
    case SHELL_LUA_EXTENSION_STATE_OUT_OF_DATE:
      return "out-of-date";
    case SHELL_LUA_EXTENSION_STATE_DOWNLOADING:
      return "downloading";
    case SHELL_LUA_EXTENSION_STATE_INITIALIZED:
      return "initialized";
    case SHELL_LUA_EXTENSION_STATE_DEACTIVATING:
      return "deactivating";
    case SHELL_LUA_EXTENSION_STATE_ACTIVATING:
      return "activating";
    case SHELL_LUA_EXTENSION_STATE_UNINSTALLED:
      return "uninstalled";
    default:
      return "unknown";
    }
}

/* test.extension_state (uuid) -> {state, error, errors}, or nil */
static int
l_extension_state (lua_State *L)
{
  const char *uuid = luaL_checkstring (L, 1);
  ShellExtensionManager *manager = check_extension_manager (L);
  ShellLuaExtension *extension = shell_extension_manager_lookup (manager, uuid);
  const char *error;

  if (extension == NULL)
    {
      lua_pushnil (L);
      return 1;
    }

  lua_createtable (L, 0, 3);
  lua_pushstring (L, state_nick (shell_lua_extension_get_state (extension)));
  lua_setfield (L, -2, "state");
  error = shell_lua_extension_get_error (extension);
  if (error != NULL && error[0] != '\0')
    {
      lua_pushstring (L, error);
      lua_setfield (L, -2, "error");
    }
  lua_pushinteger (L, shell_lua_extension_get_error_count (extension));
  lua_setfield (L, -2, "errors");
  return 1;
}

/* test.extension_report (uuid) -> {checks, passed, failed, done} */
static int
l_extension_report (lua_State *L)
{
  Runner *r = the_runner;
  const char *uuid = luaL_checkstring (L, 1);
  ExtensionReport *report = g_hash_table_lookup (r->reports, uuid);
  lua_Integer passed = 0, failed = 0;
  guint i;

  luaL_checkstack (L, 6, "extension_report");
  lua_createtable (L, 0, 4);

  lua_createtable (L, report != NULL ? report->checks->len : 0, 0);
  for (i = 0; report != NULL && i < report->checks->len; i++)
    {
      Check *check = g_ptr_array_index (report->checks, i);

      lua_createtable (L, 0, 3);
      lua_pushstring (L, check->name);
      lua_setfield (L, -2, "name");
      lua_pushboolean (L, check->ok);
      lua_setfield (L, -2, "ok");
      if (check->message != NULL)
        {
          lua_pushstring (L, check->message);
          lua_setfield (L, -2, "message");
        }
      lua_rawseti (L, -2, i + 1);

      if (check->ok)
        passed++;
      else
        failed++;
    }
  lua_setfield (L, -2, "checks");

  lua_pushinteger (L, passed);
  lua_setfield (L, -2, "passed");
  lua_pushinteger (L, failed);
  lua_setfield (L, -2, "failed");

  lua_newtable (L);
  if (report != NULL)
    {
      GHashTableIter iter;
      gpointer label;

      g_hash_table_iter_init (&iter, report->done);
      while (g_hash_table_iter_next (&iter, &label, NULL))
        {
          lua_pushboolean (L, 1);
          lua_setfield (L, -2, label);
        }
    }
  lua_setfield (L, -2, "done");
  return 1;
}

static const char *
log_level_nick (GLogLevelFlags level)
{
  if (level & G_LOG_LEVEL_ERROR)
    return "error";
  if (level & G_LOG_LEVEL_CRITICAL)
    return "critical";
  if (level & G_LOG_LEVEL_WARNING)
    return "warning";
  if (level & G_LOG_LEVEL_MESSAGE)
    return "message";
  if (level & G_LOG_LEVEL_INFO)
    return "info";
  return "debug";
}

static void
capture_log_handler (const char     *domain,
                     GLogLevelFlags  level,
                     const char     *message,
                     gpointer        user_data)
{
  Runner *r = user_data;
  CapturedLog *entry;

  /* The runtime logs the errors of the callbacks as "[uuid] message" in its
   * own domain; the other messages of that domain are not the extension's */
  if (g_str_equal (domain, "ShellLua"))
    {
      g_autofree char *prefix = g_strdup_printf ("[%s] ", r->capture_uuid);

      if (!g_str_has_prefix (message, prefix))
        {
          g_log_default_handler (domain, level, message, NULL);
          return;
        }
    }

  entry = g_new0 (CapturedLog, 1);
  entry->level = log_level_nick (level);
  entry->message = g_strdup (message);
  g_ptr_array_add (r->captured, entry);

  g_log_default_handler (domain, level, message, NULL);
}

static void
stop_log_capture (Runner *r)
{
  g_autofree char *domain = NULL;

  if (r->capture_handler == 0)
    return;

  domain = g_strconcat ("shell-lua/", r->capture_uuid, NULL);
  g_log_remove_handler (domain, r->capture_handler);
  g_log_remove_handler ("ShellLua", r->capture_lib_handler);
  g_log_set_always_fatal (r->capture_fatal);
  r->capture_handler = 0;
  r->capture_lib_handler = 0;
  g_clear_pointer (&r->capture_uuid, g_free);
}

/* test.log_capture (uuid, on): with @on the log of the extension (what it
 * logs, and the errors of its callbacks that the runtime logs) is collected,
 * and warnings and criticals are not fatal until it is stopped (the API logs
 * them on purpose); without, returns {{level, message}...} */
static int
l_log_capture (lua_State *L)
{
  Runner *r = the_runner;
  const char *uuid = luaL_checkstring (L, 1);
  guint i;

  luaL_checktype (L, 2, LUA_TBOOLEAN);

  if (lua_toboolean (L, 2))
    {
      g_autofree char *domain = NULL;
      GLogLevelFlags flags = G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION;

      if (r->capture_handler != 0)
        return luaL_error (L, "the log of an extension is already captured");

      g_ptr_array_set_size (r->captured, 0);
      r->capture_uuid = g_strdup (uuid);
      domain = g_strconcat ("shell-lua/", uuid, NULL);
      r->capture_handler = g_log_set_handler (domain, flags, capture_log_handler, r);
      r->capture_lib_handler = g_log_set_handler ("ShellLua", flags, capture_log_handler, r);
      r->capture_fatal = g_log_set_always_fatal (G_LOG_FATAL_MASK);
      return 0;
    }

  if (r->capture_handler == 0)
    return luaL_error (L, "the log is not captured");

  stop_log_capture (r);

  luaL_checkstack (L, 6, "log_capture");
  lua_createtable (L, r->captured->len, 0);
  for (i = 0; i < r->captured->len; i++)
    {
      CapturedLog *entry = g_ptr_array_index (r->captured, i);

      lua_createtable (L, 0, 2);
      lua_pushstring (L, entry->level);
      lua_setfield (L, -2, "level");
      lua_pushstring (L, entry->message);
      lua_setfield (L, -2, "message");
      lua_rawseti (L, -2, i + 1);
    }
  g_ptr_array_set_size (r->captured, 0);
  return 1;
}

static void
count_actors (ClutterActor *actor,
              GHashTable   *all,
              GHashTable   *mapped)
{
  ClutterActor *child;
  const char *name = G_OBJECT_TYPE_NAME (actor);

  g_hash_table_replace (all, (gpointer) name,
                        GINT_TO_POINTER (GPOINTER_TO_INT (g_hash_table_lookup (all, name)) + 1));
  if (clutter_actor_is_mapped (actor))
    g_hash_table_replace (mapped, (gpointer) name,
                          GINT_TO_POINTER (GPOINTER_TO_INT (g_hash_table_lookup (mapped, name)) + 1));

  for (child = clutter_actor_get_first_child (actor);
       child != NULL;
       child = clutter_actor_get_next_sibling (child))
    count_actors (child, all, mapped);
}

static void
push_counts (lua_State  *L,
             GHashTable *counts)
{
  GHashTableIter iter;
  gpointer name, count;

  lua_createtable (L, 0, g_hash_table_size (counts));
  g_hash_table_iter_init (&iter, counts);
  while (g_hash_table_iter_next (&iter, &name, &count))
    {
      lua_pushinteger (L, GPOINTER_TO_INT (count));
      lua_setfield (L, -2, name);
    }
}

/* test.fingerprint () -> {actors, mapped, listeners}: how many actors of
 * each type are in the stage (the mapped ones too) and how many listeners
 * each hook has. The shell is idle before the tests compare two of them. */
static int
l_fingerprint (lua_State *L)
{
  ShellHookRegistry *registry = shell_hook_registry_get_default ();
  g_autoptr (GHashTable) all = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (GHashTable) mapped = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (GHashTable) listeners = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_auto (GStrv) hooks = shell_hook_registry_list (registry);
  guint i;

  count_actors (CLUTTER_ACTOR (shell_global_get_stage (shell_global_get ())), all, mapped);

  for (i = 0; hooks != NULL && hooks[i] != NULL; i++)
    {
      guint n = shell_hook_registry_count_listeners (registry, hooks[i], SHELL_HOOK_PHASE_BEFORE) +
                shell_hook_registry_count_listeners (registry, hooks[i], SHELL_HOOK_PHASE_AFTER);

      g_hash_table_replace (listeners, g_strdup (hooks[i]), GUINT_TO_POINTER (n));
    }

  luaL_checkstack (L, 6, "fingerprint");
  lua_createtable (L, 0, 3);
  push_counts (L, all);
  lua_setfield (L, -2, "actors");
  push_counts (L, mapped);
  lua_setfield (L, -2, "mapped");
  push_counts (L, listeners);
  lua_setfield (L, -2, "listeners");
  return 1;
}

/* ------------------------------------------------------------------------ */
/* test.http_server                                                          */
/* ------------------------------------------------------------------------ */

/* test.http_server {routes = {["/path"] = route ...}} -> server
 *
 * A SoupServer on 127.0.0.1 and an ephemeral port, answering from the main
 * loop of the shell (the tests of shell.network have no internet). A route is
 * a string (the body of a 200 answer, text/plain) or a table:
 *
 *   status       integer, default 200 (302 with a redirect)
 *   body         string
 *   size         integer, a body of that many 'x' (instead of body)
 *   content_type string, default text/plain
 *   headers      {Name = "value"} added to the answer; a sequence of values
 *                instead of a string sends the header once for each
 *   delay_ms     integer, the answer is sent that long after the request
 *   echo         boolean, the body of the answer is the body of the request
 *                (and X-Echo-Method the method)
 *   redirect     string, a path on this server or an absolute URL: Location
 *
 * A path without a route answers 404. The server table has `url`
 * ("http://127.0.0.1:<port>", no trailing slash) and `port`, and the methods
 *
 *   server:route (path, route)   adds or replaces a route
 *   server:requests ()           the requests so far, in order: {method,
 *                                path (with the query), headers (names in
 *                                lower case), body}
 *   server:stop ()               stops listening and closes the connections
 *
 * The servers live until the end of the run. */

#define HTTP_LOG_LIMIT 1000

typedef struct
{
  int status;
  char *content_type;
  GBytes *body;
  GPtrArray *headers;   /* name, value, name, value... */
  guint delay_ms;
  gboolean echo;
  char *redirect;
} HttpRoute;

typedef struct
{
  char *method;
  char *path;
  GPtrArray *headers;   /* name (lower case), value, ... */
  GBytes *body;
} HttpRequestLog;

typedef struct
{
  SoupServer *server;
  GHashTable *routes;   /* path -> HttpRoute */
  GPtrArray *log;       /* HttpRequestLog */
  char *url;
  gboolean stopped;
} HttpServer;

typedef struct
{
  SoupServerMessage *message;   /* strong while the answer is held back */
  guint source;
  gulong disconnected_id;
} HttpDelay;

static GPtrArray *http_servers = NULL;   /* HttpServer, until the end of the run */

static void
http_route_free (gpointer data)
{
  HttpRoute *route = data;

  g_free (route->content_type);
  g_clear_pointer (&route->body, g_bytes_unref);
  g_clear_pointer (&route->headers, g_ptr_array_unref);
  g_free (route->redirect);
  g_free (route);
}

static void
http_request_log_free (gpointer data)
{
  HttpRequestLog *entry = data;

  g_free (entry->method);
  g_free (entry->path);
  g_clear_pointer (&entry->headers, g_ptr_array_unref);
  g_clear_pointer (&entry->body, g_bytes_unref);
  g_free (entry);
}

static void
http_server_free (gpointer data)
{
  HttpServer *srv = data;

  if (srv->server != NULL)
    {
      soup_server_disconnect (srv->server);
      g_clear_object (&srv->server);
    }
  g_hash_table_unref (srv->routes);
  g_ptr_array_unref (srv->log);
  g_free (srv->url);
  g_free (srv);
}

/* Called when the run ends (runner_free ()) */
static void
http_servers_free (void)
{
  g_clear_pointer (&http_servers, g_ptr_array_unref);
}

static void
http_delay_free (HttpDelay *delay)
{
  g_clear_handle_id (&delay->source, g_source_remove);
  if (delay->disconnected_id != 0)
    g_signal_handler_disconnect (delay->message, delay->disconnected_id);
  g_clear_object (&delay->message);
  g_free (delay);
}

static gboolean
http_delay_done_cb (gpointer user_data)
{
  HttpDelay *delay = user_data;

  delay->source = 0;
  soup_server_message_unpause (delay->message);
  http_delay_free (delay);
  return G_SOURCE_REMOVE;
}

/* The client went away (a timeout or a cancel in the test): nothing to answer */
static void
http_delay_disconnected_cb (SoupServerMessage *message,
                            gpointer           user_data)
{
  HttpDelay *delay = user_data;

  delay->disconnected_id = 0;   /* the signal handler goes with the emission */
  http_delay_free (delay);
}

static void
http_header_pairs_append (GPtrArray  *pairs,
                          const char *name,
                          const char *value)
{
  g_ptr_array_add (pairs, g_strdup (name));
  g_ptr_array_add (pairs, g_strdup (value));
}

static void
http_log_header_cb (const char *name,
                    const char *value,
                    gpointer    user_data)
{
  GPtrArray *pairs = user_data;
  g_autofree char *lower = g_ascii_strdown (name, -1);

  http_header_pairs_append (pairs, lower, value);
}

static void
http_handler_cb (SoupServer        *server,
                 SoupServerMessage *message,
                 const char        *path,
                 GHashTable        *query,
                 gpointer           user_data)
{
  HttpServer *srv = user_data;
  SoupMessageHeaders *request_headers = soup_server_message_get_request_headers (message);
  SoupMessageHeaders *response_headers = soup_server_message_get_response_headers (message);
  SoupMessageBody *request_body = soup_server_message_get_request_body (message);
  GUri *uri = soup_server_message_get_uri (message);
  const char *method = soup_server_message_get_method (message);
  g_autoptr (GBytes) body = g_bytes_new (request_body->data, request_body->length);
  HttpRequestLog *entry;
  HttpRoute *route;
  const char *content_type = "text/plain";
  g_autoptr (GBytes) answer = NULL;
  guint i;

  entry = g_new0 (HttpRequestLog, 1);
  entry->method = g_strdup (method);
  entry->path = g_uri_get_query (uri) != NULL ?
    g_strdup_printf ("%s?%s", g_uri_get_path (uri), g_uri_get_query (uri)) : g_strdup (g_uri_get_path (uri));
  entry->headers = g_ptr_array_new_with_free_func (g_free);
  soup_message_headers_foreach (request_headers, http_log_header_cb, entry->headers);
  entry->body = g_bytes_ref (body);
  if (srv->log->len >= HTTP_LOG_LIMIT)
    g_ptr_array_remove_index (srv->log, 0);
  g_ptr_array_add (srv->log, entry);

  route = g_hash_table_lookup (srv->routes, path);
  if (route == NULL)
    {
      soup_server_message_set_status (message, SOUP_STATUS_NOT_FOUND, NULL);
      soup_server_message_set_response (message, "text/plain", SOUP_MEMORY_COPY, "not found\n", 10);
      return;
    }

  soup_server_message_set_status (message, (guint) route->status, NULL);
  if (route->redirect != NULL)
    {
      g_autofree char *location = strstr (route->redirect, "://") != NULL ?
        g_strdup (route->redirect) : g_strconcat (srv->url, route->redirect, NULL);

      soup_message_headers_replace (response_headers, "Location", location);
    }
  for (i = 0; i + 1 < route->headers->len; i += 2)
    soup_message_headers_append (response_headers, g_ptr_array_index (route->headers, i),
                                 g_ptr_array_index (route->headers, i + 1));

  if (route->echo)
    {
      const char *request_type = soup_message_headers_get_content_type (request_headers, NULL);

      answer = g_bytes_ref (body);
      if (request_type != NULL)
        content_type = request_type;
      soup_message_headers_replace (response_headers, "X-Echo-Method", method);
    }
  else
    {
      answer = g_bytes_ref (route->body);
      content_type = route->content_type;
    }

  /* A HEAD answer has the headers of the GET and no body: libsoup drops it */
  soup_server_message_set_response (message, content_type, SOUP_MEMORY_COPY,
                                    g_bytes_get_size (answer) > 0 ? g_bytes_get_data (answer, NULL) : "",
                                    g_bytes_get_size (answer));

  if (route->delay_ms > 0)
    {
      HttpDelay *delay = g_new0 (HttpDelay, 1);

      delay->message = g_object_ref (message);
      soup_server_message_pause (message);
      delay->source = g_timeout_add (route->delay_ms, http_delay_done_cb, delay);
      delay->disconnected_id = g_signal_connect (message, "disconnected",
                                                 G_CALLBACK (http_delay_disconnected_cb), delay);
    }
}

/* The route (a string or a table) at @idx of the stack, to a new HttpRoute
 * (raises) */
static HttpRoute *
http_route_from_lua (lua_State *L,
                     int        idx)
{
  HttpRoute *route;
  const char *content_type = "text/plain", *redirect = NULL;
  const char *body = "";
  size_t body_len = 0;
  lua_Integer status = 0, delay_ms = 0, size = -1;
  gboolean echo = FALSE;
  int headers_idx = 0;

  idx = lua_absindex (L, idx);

  if (lua_type (L, idx) == LUA_TSTRING)
    {
      body = lua_tolstring (L, idx, &body_len);
    }
  else
    {
      luaL_checktype (L, idx, LUA_TTABLE);

      if (lua_getfield (L, idx, "status") != LUA_TNIL)
        status = luaL_checkinteger (L, -1);
      lua_pop (L, 1);
      if (lua_getfield (L, idx, "body") != LUA_TNIL)
        body = luaL_checklstring (L, -1, &body_len);
      lua_pop (L, 1);   /* the table keeps the string alive */
      if (lua_getfield (L, idx, "size") != LUA_TNIL)
        size = luaL_checkinteger (L, -1);
      lua_pop (L, 1);
      if (lua_getfield (L, idx, "content_type") != LUA_TNIL)
        content_type = luaL_checkstring (L, -1);
      lua_pop (L, 1);
      if (lua_getfield (L, idx, "delay_ms") != LUA_TNIL)
        delay_ms = luaL_checkinteger (L, -1);
      lua_pop (L, 1);
      if (lua_getfield (L, idx, "echo") != LUA_TNIL)
        echo = lua_toboolean (L, -1);
      lua_pop (L, 1);
      if (lua_getfield (L, idx, "redirect") != LUA_TNIL)
        redirect = luaL_checkstring (L, -1);
      lua_pop (L, 1);
      if (lua_getfield (L, idx, "headers") != LUA_TNIL)
        {
          luaL_checktype (L, -1, LUA_TTABLE);
          headers_idx = lua_gettop (L);
        }
      else
        {
          lua_pop (L, 1);
        }
    }

  if (status == 0)
    status = redirect != NULL ? 302 : 200;
  if (status < 100 || status > 599 || delay_ms < 0 || size > 256 * 1024 * 1024)
    luaL_error (L, "a route with a status, delay_ms or size out of range");

  route = g_new0 (HttpRoute, 1);
  route->status = (int) status;
  route->content_type = g_strdup (content_type);
  route->delay_ms = (guint) delay_ms;
  route->echo = echo;
  route->redirect = g_strdup (redirect);
  route->headers = g_ptr_array_new_with_free_func (g_free);
  if (size >= 0)
    {
      char *data = g_malloc ((gsize) size + 1);

      memset (data, 'x', (gsize) size);
      route->body = g_bytes_new_take (data, (gsize) size);
    }
  else
    {
      route->body = g_bytes_new (body, body_len);
    }

  if (headers_idx != 0)
    {
      lua_pushnil (L);
      while (lua_next (L, headers_idx) != 0)
        {
          if (lua_type (L, -2) == LUA_TSTRING && lua_type (L, -1) == LUA_TSTRING)
            {
              http_header_pairs_append (route->headers, lua_tostring (L, -2), lua_tostring (L, -1));
            }
          else if (lua_type (L, -2) == LUA_TSTRING && lua_type (L, -1) == LUA_TTABLE)
            {
              /* a sequence of values: the header is sent once for each */
              lua_Unsigned n = lua_rawlen (L, -1), k;

              for (k = 1; k <= n; k++)
                {
                  if (lua_rawgeti (L, -1, (lua_Integer) k) == LUA_TSTRING)
                    http_header_pairs_append (route->headers, lua_tostring (L, -3), lua_tostring (L, -1));
                  lua_pop (L, 1);
                }
            }
          lua_pop (L, 1);
        }
      lua_pop (L, 1);   /* the table */
    }

  return route;
}

static HttpServer *
http_server_upvalue (lua_State *L)
{
  return lua_touserdata (L, lua_upvalueindex (1));
}

/* server:route (path, route) */
static int
l_http_route (lua_State *L)
{
  HttpServer *srv = http_server_upvalue (L);
  const char *path = luaL_checkstring (L, 2);
  HttpRoute *route = http_route_from_lua (L, 3);

  g_hash_table_replace (srv->routes, g_strdup (path), route);
  return 0;
}

/* server:requests () */
static int
l_http_requests (lua_State *L)
{
  HttpServer *srv = http_server_upvalue (L);
  guint i, j;

  lua_createtable (L, (int) srv->log->len, 0);
  for (i = 0; i < srv->log->len; i++)
    {
      HttpRequestLog *entry = g_ptr_array_index (srv->log, i);
      gsize size;
      gconstpointer data = g_bytes_get_data (entry->body, &size);

      lua_createtable (L, 0, 4);
      lua_pushstring (L, entry->method);
      lua_setfield (L, -2, "method");
      lua_pushstring (L, entry->path);
      lua_setfield (L, -2, "path");
      lua_newtable (L);
      for (j = 0; j + 1 < entry->headers->len; j += 2)
        {
          const char *name = g_ptr_array_index (entry->headers, j);
          const char *value = g_ptr_array_index (entry->headers, j + 1);

          if (lua_getfield (L, -1, name) == LUA_TSTRING)
            lua_pushfstring (L, "%s, %s", lua_tostring (L, -1), value);
          else
            lua_pushstring (L, value);
          lua_setfield (L, -3, name);
          lua_pop (L, 1);
        }
      lua_setfield (L, -2, "headers");
      lua_pushlstring (L, size > 0 ? data : "", size);
      lua_setfield (L, -2, "body");
      lua_rawseti (L, -2, (lua_Integer) i + 1);
    }
  return 1;
}

/* server:stop () */
static int
l_http_stop (lua_State *L)
{
  HttpServer *srv = http_server_upvalue (L);

  if (!srv->stopped && srv->server != NULL)
    {
      srv->stopped = TRUE;
      soup_server_disconnect (srv->server);
    }
  return 0;
}

/* test.http_server {routes = {...}} */
static int
l_http_server (lua_State *L)
{
  HttpServer *srv;
  SoupServer *server;
  GError *error = NULL;
  GSList *uris;
  GUri *uri;

  luaL_checktype (L, 1, LUA_TTABLE);

  server = soup_server_new ("server-header", "gnome-lua-test", NULL);
  if (!soup_server_listen_local (server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error))
    {
      lua_pushfstring (L, "cannot listen on 127.0.0.1: %s", error->message);
      g_error_free (error);
      g_object_unref (server);
      return lua_error (L);
    }
  uris = soup_server_get_uris (server);
  if (uris == NULL)
    {
      g_object_unref (server);
      return luaL_error (L, "the server has no address");
    }
  uri = uris->data;

  srv = g_new0 (HttpServer, 1);
  srv->server = server;
  srv->routes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, http_route_free);
  srv->log = g_ptr_array_new_with_free_func (http_request_log_free);
  srv->url = g_strdup_printf ("http://127.0.0.1:%d", g_uri_get_port (uri));
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);

  if (http_servers == NULL)
    http_servers = g_ptr_array_new_with_free_func (http_server_free);
  g_ptr_array_add (http_servers, srv);

  if (lua_getfield (L, 1, "routes") != LUA_TNIL)
    {
      luaL_checktype (L, -1, LUA_TTABLE);
      lua_pushnil (L);
      while (lua_next (L, -2) != 0)
        {
          const char *path;

          if (lua_type (L, -2) != LUA_TSTRING)
            return luaL_error (L, "the keys of 'routes' must be paths");
          path = lua_tostring (L, -2);
          g_hash_table_replace (srv->routes, g_strdup (path), http_route_from_lua (L, -1));
          lua_pop (L, 1);
        }
    }
  lua_pop (L, 1);

  soup_server_add_handler (srv->server, NULL, http_handler_cb, srv, NULL);

  lua_createtable (L, 0, 5);
  lua_pushstring (L, srv->url);
  lua_setfield (L, -2, "url");
  lua_pushinteger (L, strtol (strrchr (srv->url, ':') + 1, NULL, 10));
  lua_setfield (L, -2, "port");
  lua_pushlightuserdata (L, srv);
  lua_pushcclosure (L, l_http_route, 1);
  lua_setfield (L, -2, "route");
  lua_pushlightuserdata (L, srv);
  lua_pushcclosure (L, l_http_requests, 1);
  lua_setfield (L, -2, "requests");
  lua_pushlightuserdata (L, srv);
  lua_pushcclosure (L, l_http_stop, 1);
  lua_setfield (L, -2, "stop");
  return 1;
}

/* ------------------------------------------------------------------------ */
/* The test module and the script                                             */
/* ------------------------------------------------------------------------ */

static const luaL_Reg test_functions[] = {
  { "sleep", l_sleep },
  { "wait_leisure", l_wait_leisure },
  { "wait_startup", l_wait_startup },
  { "wait_and_draw", l_wait_and_draw },
  { "watch", l_watch },
  { "wait", l_wait },
  { "create_window", l_create_window },
  { "wait_windows", l_wait_windows },
  { "destroy_windows", l_destroy_windows },
  { "disable_helper_auto_exit", l_disable_helper_auto_exit },
  { "timeout", l_timeout },
  { "now", l_now },
  { "new", l_new },
  { "new_monitor", l_new_monitor },
  { "create_virtual_device", l_create_virtual_device },
  { "press_accelerator", l_press_accelerator },
  { "set_boolean", l_set_boolean },
  { "extension_enable", l_extension_enable },
  { "extension_disable", l_extension_disable },
  { "extension_state", l_extension_state },
  { "extension_report", l_extension_report },
  { "log_capture", l_log_capture },
  { "fingerprint", l_fingerprint },
  { "http_server", l_http_server },
  { "print", l_print },
  { "log", l_log },
  { "define_event", l_define_event },
  { "event", l_event },
  { "event_i", l_event_i },
  { "collect_statistics", l_collect_statistics },
  { "events", l_events },
  { "write_report", l_write_report },
  { NULL, NULL }
};

static const luaL_Reg monitor_methods[] = {
  { "destroy", l_monitor_destroy },
  { NULL, NULL }
};

/* The parts of the module that are easier in Lua */
static const char prelude[] =
"local test = test\n"
"local handlers, finishers, metrics = {}, {}, {}\n"
"\n"
"print = test.print\n"
"\n"
"function test.on_event (name, fn)\n"
"  handlers[name] = fn\n"
"end\n"
"\n"
"function test.on_script_event (name, fn)\n"
"  handlers['script.' .. name] = fn\n"
"end\n"
"\n"
"function test.finish (fn)\n"
"  finishers[#finishers + 1] = fn\n"
"end\n"
"\n"
"function test.metrics (table)\n"
"  metrics = table\n"
"  return table\n"
"end\n"
"\n"
"function test.expect_equal (a, b, description)\n"
"  if a ~= b then\n"
"    error ((description and (description .. ' ') or '') .. tostring (a) ..\n"
"           ' does not match ' .. tostring (b), 2)\n"
"  end\n"
"end\n"
"\n"
"function test.wait_signal (object, name, timeout)\n"
"  test.wait (test.watch (object, name), timeout)\n"
"end\n"
"\n"
"function test.wait_for (predicate, description, timeout)\n"
"  local start = test.now ()\n"
"  timeout = timeout or 10000\n"
"  while not predicate () do\n"
"    if test.now () - start > timeout then\n"
"      error ('Timeout waiting for ' .. description, 2)\n"
"    end\n"
"    test.sleep (100)\n"
"  end\n"
"end\n"
"\n"
"local escapes = { ['\"'] = '\\\\\"', ['\\\\'] = '\\\\\\\\', ['\\n'] = '\\\\n',\n"
"                  ['\\r'] = '\\\\r', ['\\t'] = '\\\\t', ['\\b'] = '\\\\b',\n"
"                  ['\\f'] = '\\\\f' }\n"
"\n"
"local function json_string (s)\n"
"  local escaped = tostring (s):gsub ('[%c\"\\\\]', function (c)\n"
"    return escapes[c] or string.format ('\\\\u%04x', c:byte ())\n"
"  end)\n"
"  return '\"' .. escaped .. '\"'\n"
"end\n"
"\n"
"local function json_value (v)\n"
"  if type (v) == 'string' then\n"
"    return json_string (v)\n"
"  elseif math.type (v) == 'integer' then\n"
"    return string.format ('%d', v)\n"
"  elseif type (v) == 'number' then\n"
"    if v ~= v or v == math.huge or v == -math.huge then\n"
"      return 'null'\n"
"    end\n"
"    return string.format ('%.17g', v)\n"
"  end\n"
"  return tostring (v)\n"
"end\n"
"\n"
"local function report ()\n"
"  local names = {}\n"
"  for name in pairs (metrics) do\n"
"    names[#names + 1] = name\n"
"  end\n"
"  table.sort (names)\n"
"\n"
"  local output = os.getenv ('SHELL_PERF_OUTPUT')\n"
"  if output and output ~= '' then\n"
"    local entries = {}\n"
"    for _, name in ipairs (names) do\n"
"      local metric = metrics[name]\n"
"      if metric.description == nil then\n"
"        print ('Error: No description found for metric ' .. name)\n"
"      elseif metric.units == nil then\n"
"        print ('Error: No units found for metric ' .. name)\n"
"      elseif metric.value == nil then\n"
"        print ('Error: No value found for metric ' .. name)\n"
"      else\n"
"        entries[#entries + 1] = string.format (\n"
"          '{ \"name\": %s,\\n    \"description\": %s,\\n    \"units\": %s,\\n    \"value\": %s }',\n"
"          json_string (name), json_string (metric.description),\n"
"          json_string (metric.units), json_value (metric.value))\n"
"      end\n"
"    end\n"
"    test.write_report (output, table.concat (entries, ',\\n  '))\n"
"  else\n"
"    print ('------------------------------------------------------------')\n"
"    for _, name in ipairs (names) do\n"
"      local metric = metrics[name]\n"
"      print ('# ' .. tostring (metric.description))\n"
"      print (name .. ': ' .. tostring (metric.value) .. tostring (metric.units))\n"
"    end\n"
"    print ('------------------------------------------------------------')\n"
"  end\n"
"end\n"
"\n"
"-- Called when the script is done: replays the performance log to the\n"
"-- handlers, then the finish functions, then reports the metrics\n"
"function test.__collect ()\n"
"  for _, event in ipairs (test.events ()) do\n"
"    local handler = handlers[event.name]\n"
"    if handler then\n"
"      handler (event.time, event.arg)\n"
"    end\n"
"  end\n"
"  for _, finisher in ipairs (finishers) do\n"
"    finisher ()\n"
"  end\n"
"  report ()\n"
"end\n";

static int
l_boot (lua_State *L)
{
  Runner *r = the_runner;

  luaL_checkstack (L, 12, "boot");

  lua_rawgeti (L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
  r->L = lua_tothread (L, -1);
  lua_pop (L, 1);

  luaL_newmetatable (L, WAITER_METATABLE);
  lua_pop (L, 1);

  luaL_newmetatable (L, MONITOR_METATABLE);
  lua_newtable (L);
  luaL_setfuncs (L, monitor_methods, 0);
  lua_setfield (L, -2, "__index");
  lua_pop (L, 1);

  lua_newtable (L);
  luaL_setfuncs (L, test_functions, 0);
  lua_setglobal (L, "test");

  if (luaL_loadbufferx (L, prelude, sizeof (prelude) - 1, "=(test prelude)", "t") != LUA_OK)
    return lua_error (L);
  lua_call (L, 0, 0);

  /* The script, as a coroutine of its own: stack is [chunk] -> [] */
  if (luaL_loadfile (L, r->script) != LUA_OK)
    return lua_error (L);

  r->co = lua_newthread (L);
  lua_pushvalue (L, -1);
  r->co_ref = luaL_ref (L, LUA_REGISTRYINDEX);
  lua_pop (L, 1);
  lua_xmove (L, r->co, 1);

  return 0;
}

static gboolean
first_resume_cb (gpointer data)
{
  Runner *r = the_runner;

  if (r != NULL && r->parked && GPOINTER_TO_UINT (data) == r->park_serial)
    resume_script (r);

  return G_SOURCE_REMOVE;
}

static void
script_finished (Runner *r)
{
  gboolean helper_exit;

  if (!lua_checkstack (r->L, 4))
    {
      runner_fail (r, "out of stack space");
      return;
    }

  lua_getglobal (r->L, "test");
  lua_getfield (r->L, -1, "__collect");
  lua_remove (r->L, -2);
  if (!call_in_thread (r, 0, NULL))
    return;

  /* The helper exits with the script, unless the script wants its windows to
   * remain during the exit of the shell */
  helper_exit = r->helper != NULL && r->helper_auto_exit;
  if (helper_exit && r->helper_ready)
    {
      g_dbus_connection_call (r->bus, PERF_HELPER_NAME, PERF_HELPER_PATH, PERF_HELPER_NAME,
                              "Exit", g_variant_new ("()"), NULL, G_DBUS_CALL_FLAGS_NONE,
                              -1, r->cancellable, helper_exit_done_cb, NULL);
      return;
    }

  runner_finish (r, NULL);
}

static void
boot (Runner *r)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (ShellLayoutManager) layout_manager = NULL;
  ShellGlobal *global = shell_global_get ();
  ShellMain *main_object = shell_main_get_default ();
  g_autofree char *result = NULL;

  register_object_methods ();

  shell_perf_log_set_enabled (shell_perf_log_get_default (), TRUE);

  r->inspector = g_object_new (SHELL_TYPE_LUA_INSPECTOR, NULL);
  shell_lua_inspector_add_function (r->inspector, "__test_boot", l_boot);
  shell_lua_inspector_set_global_object (r->inspector, "global", G_OBJECT (global));
  shell_lua_inspector_set_global_object (r->inspector, "stage",
                                         G_OBJECT (shell_global_get_stage (global)));
  shell_lua_inspector_set_global_object (r->inspector, "main", G_OBJECT (main_object));

  /* The startup is complete once the layout manager says so, or when the
   * action mode left NONE (ShellMain sets it then) if we missed that */
  layout_manager = g_object_ref (shell_main_get_layout_manager (main_object));
  g_weak_ref_set (&r->layout_manager, layout_manager);
  r->startup_id = g_signal_connect (layout_manager, "startup-complete",
                                    G_CALLBACK (startup_complete_cb), r);
  r->startup_complete = shell_main_get_action_mode (main_object) != SHELL_ACTION_MODE_NONE;

  result = shell_lua_inspector_eval (r->inspector, "__test_boot ()", &error);
  if (result == NULL)
    {
      runner_fail (r, "Could not load %s: %s", r->script, error->message);
      return;
    }

  /* Start the script from the main loop: it runs outside of the evaluation */
  r->parked = TRUE;
  r->park_serial++;
  g_idle_add (first_resume_cb, GUINT_TO_POINTER (r->park_serial));
}

static gboolean
poll_ready_cb (gpointer data)
{
  Runner *r = data;
  ShellMain *main_object = shell_main_get_default ();

  if (shell_global_get_stage (shell_global_get ()) == NULL ||
      shell_main_get_layout_manager (main_object) == NULL ||
      shell_main_get_window_manager (main_object) == NULL ||
      shell_main_get_overview (main_object) == NULL ||
      shell_main_get_message_tray (main_object) == NULL ||
      shell_main_get_panel (main_object) == NULL)
    return G_SOURCE_CONTINUE;

  r->poll_id = 0;
  boot (r);
  return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------------ */
/* Teardown                                                                  */
/* ------------------------------------------------------------------------ */

static void
runner_free (Runner *r)
{
  guint i;

  r->finished = TRUE;
  g_clear_handle_id (&r->poll_id, g_source_remove);
  g_clear_handle_id (&r->wake_source, g_source_remove);

  if (r->cancellable != NULL)
    g_cancellable_cancel (r->cancellable);

  for (i = 0; i < r->waiters->len; i++)
    waiter_disconnect (g_ptr_array_index (r->waiters, i));

  if (r->startup_id != 0)
    {
      g_autoptr (GObject) layout_manager = g_weak_ref_get (&r->layout_manager);

      if (layout_manager != NULL)
        g_signal_handler_disconnect (layout_manager, r->startup_id);
      r->startup_id = 0;
    }
  g_weak_ref_clear (&r->layout_manager);

  /* What the script made, while the state can still be told */
  for (i = 0; i < r->monitors->len; i++)
    destroy_monitor (r, g_ptr_array_index (r->monitors, i));
  g_ptr_array_set_size (r->keep, 0);
  stop_log_capture (r);

  /* The state is closed with the inspector: from now on the closures and the
   * callbacks that are left do nothing */
  the_runner = NULL;
  r->L = NULL;
  r->co = NULL;
  g_clear_object (&r->inspector);

  if (r->helper_watch_id != 0)
    g_bus_unwatch_name (r->helper_watch_id);
  g_clear_object (&r->bus);
  g_clear_object (&r->helper);
  g_clear_object (&r->cancellable);
  g_clear_pointer (&r->pending_params, g_variant_unref);
  g_free (r->pending_method);

  g_ptr_array_unref (r->monitors);
  g_ptr_array_unref (r->keep);
  g_ptr_array_unref (r->waiters);
  g_hash_table_unref (r->reports);
  g_ptr_array_unref (r->captured);
  http_servers_free ();
  g_free (r->script);
  g_free (r);
}

static void
shutdown_cb (ShellGlobal *global,
             gpointer     user_data)
{
  Runner *r = user_data;

  g_signal_handler_disconnect (global, r->shutdown_id);
  r->shutdown_id = 0;
  runner_free (r);
}

static void
test_monitor_free (TestMonitor *monitor)
{
  g_free (monitor);
}

void
shell_test_runner_start (void)
{
  const char *script = g_getenv (SCRIPT_ENV);
  g_autofree char *absolute = NULL;
  Runner *r;

  if (script == NULL || script[0] == '\0' || the_runner != NULL)
    return;

  if (!g_path_is_absolute (script))
    {
      g_autofree char *cwd = g_get_current_dir ();

      absolute = g_build_filename (cwd, script, NULL);
    }
  else
    {
      absolute = g_strdup (script);
    }

  r = g_new0 (Runner, 1);
  r->script = g_steal_pointer (&absolute);
  r->helper_auto_exit = TRUE;
  r->cancellable = g_cancellable_new ();
  r->keep = g_ptr_array_new_with_free_func (g_object_unref);
  r->monitors = g_ptr_array_new_with_free_func ((GDestroyNotify) test_monitor_free);
  r->waiters = g_ptr_array_new_with_free_func ((GDestroyNotify) waiter_free);
  r->reports = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                      (GDestroyNotify) extension_report_free);
  r->captured = g_ptr_array_new_with_free_func ((GDestroyNotify) captured_log_free);
  g_weak_ref_init (&r->layout_manager, NULL);
  the_runner = r;

  /* What the fixture extensions of tests/api use to report and to be driven */
  shell_lua_api_register ("test", api_test_register);
  shell_hook_registry_declare (shell_hook_registry_get_default (), STEP_HOOK, "step:s", FALSE,
                               "Test runner: asks the fixture extensions to run a step (tests/api)");

  r->shutdown_id = g_signal_connect_after (shell_global_get (), "shutdown",
                                           G_CALLBACK (shutdown_cb), r);
  r->poll_id = g_timeout_add (POLL_INTERVAL_MS, poll_ready_cb, r);
  g_source_set_name_by_id (r->poll_id, "[gnome-shell] test runner start");
}
