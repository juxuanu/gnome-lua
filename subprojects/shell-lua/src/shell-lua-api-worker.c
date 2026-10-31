/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * shell.worker (docs/api/worker.md): Lua code in a thread of its own, gated
 * by the "worker" capability.
 *
 * Notes for maintainers:
 *
 *  - A worker is a ShellLuaWorker, an owned object of the extension
 *    (shell_lua_own_object), and a thread. The thread creates a Lua state of
 *    kind SHELL_LUA_STATE_WORKER, loads the module, handles the messages of
 *    the inbox one by one and closes the state before it exits. The thread
 *    is the only one that ever touches that state; the main thread reaches
 *    it through two things only: the queues (under the lock) and the stop
 *    flag (shell_lua_state_request_stop, atomic).
 *  - Both queues carry GVariants. The inbox (extension -> worker) refuses
 *    the 65th message (post raises); the outbox (worker -> extension) makes
 *    the worker wait for room, and a stopped worker is woken and raises
 *    there. The outbox also carries the relayed log lines and the end of
 *    the worker, so that everything arrives in order.
 *  - The main side drains the outbox from a GSource of the default context,
 *    one item per dispatch (a burst of results is spread over frames), woken
 *    by the thread with g_source_set_ready_time () under the worker's lock;
 *    the same lock orders it with the dispatch, so no wakeup is lost.
 *  - The main thread never waits for the thread: terminate (), destroy () and
 *    disable set the stop flag and return. The thread finishes its current
 *    C call, unwinds, closes its state, hands the limit slot back and drops
 *    its reference. The worker object is reference counted by both sides.
 *  - The callbacks are registry references kept here (as shell.spawn does),
 *    not ShellLuaCallbacks bound to the object: terminate () destroys the
 *    object at once and still runs on_error later, from the main loop.
 *  - luaL_error () longjmps: nothing allocated is live when it is called,
 *    and the lock is never held across it.
 */

#ifndef G_LOG_DOMAIN
#define G_LOG_DOMAIN "ShellLua"
#endif

#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "shell-lua-api-private.h"

#define MAX_WORKERS_PER_STATE 4
#define DEFAULT_MAX_MEMORY ((gsize) 64 * 1024 * 1024)
#define MIN_MAX_MEMORY ((gsize) 1024 * 1024)
#define MAX_MAX_MEMORY ((gsize) 128 * 1024 * 1024)
#define MAX_MESSAGE_BYTES ((gsize) 16 * 1024 * 1024)
#define MAX_MESSAGE_VALUES ((gsize) 1000000)
#define QUEUE_LIMIT 64
#define WORKER_NICE 10
#define STOPPED_MESSAGE "worker: stopped"

#define GROUP_KEY "shell-lua-worker-group"

static void __attribute__ ((constructor))
shell_lua_api_worker_init (void)
{
  shell_lua_api_register_for_kinds ("worker", shell_lua_api_worker_register,
                                    SHELL_LUA_API_KINDS_ALL);
}

/* ------------------------------------------------------------------------ */
/* Limits shared by the shell                                                */
/* ------------------------------------------------------------------------ */

static gint live_workers = 0;        /* threads that have not exited */
static gint worker_limit = 0;        /* 0: not computed yet */
static gsize max_message_values = MAX_MESSAGE_VALUES;   /* tests lower it */

guint
shell_lua_worker_get_limit (void)
{
  gint limit = g_atomic_int_get (&worker_limit);

  if (limit == 0)
    {
      limit = (gint) MAX (8u, 2 * g_get_num_processors ());
      g_atomic_int_compare_and_exchange (&worker_limit, 0, limit);
      limit = g_atomic_int_get (&worker_limit);
    }
  return (guint) limit;
}

void
shell_lua_worker_set_limit (guint limit)
{
  g_atomic_int_set (&worker_limit, (gint) limit);
}

guint
shell_lua_worker_get_live_count (void)
{
  return (guint) g_atomic_int_get (&live_workers);
}

void
shell_lua_worker_set_max_message_values (gsize max_values)
{
  max_message_values = max_values != 0 ? max_values : MAX_MESSAGE_VALUES;
}

/* One of the shell-wide slots, or FALSE when they are all taken. */
static gboolean
reserve_slot (void)
{
  gint limit = (gint) shell_lua_worker_get_limit ();
  gint n;

  do
    {
      n = g_atomic_int_get (&live_workers);
      if (n >= limit)
        return FALSE;
    }
  while (!g_atomic_int_compare_and_exchange (&live_workers, n, n + 1));

  return TRUE;
}

static void
release_slot (void)
{
  g_atomic_int_add (&live_workers, -1);
}

/* ------------------------------------------------------------------------ */
/* The workers of one state                                                  */
/* ------------------------------------------------------------------------ */

/* Attached to the state that starts workers; referenced by each of its
 * workers, so that a thread that exits after the state is gone still has
 * somewhere to hand its slot back. */
typedef struct {
  gint     ref_count;
  gint     live;          /* atomic: the state's workers whose thread runs */
  guint    next_id;       /* main thread */
  GMutex   lock;          /* the statistics */
  guint64  messages;
  gint64   total_us;
  gint64   longest_us;
} Group;

static Group *
group_ref (Group *group)
{
  g_atomic_int_inc (&group->ref_count);
  return group;
}

static void
group_unref (Group *group)
{
  if (g_atomic_int_dec_and_test (&group->ref_count))
    {
      g_mutex_clear (&group->lock);
      g_free (group);
    }
}

static Group *
group_for_state (ShellLuaState *state)
{
  Group *group = g_object_get_data (G_OBJECT (state), GROUP_KEY);

  if (group == NULL)
    {
      group = g_new0 (Group, 1);
      group->ref_count = 1;
      g_mutex_init (&group->lock);
      g_object_set_data_full (G_OBJECT (state), GROUP_KEY, group, (GDestroyNotify) group_unref);
    }
  return group;
}

static void
group_account (Group  *group,
               gint64  us)
{
  g_mutex_lock (&group->lock);
  group->messages++;
  group->total_us += us;
  group->longest_us = MAX (group->longest_us, us);
  g_mutex_unlock (&group->lock);
}

void
shell_lua_worker_get_stats (ShellLuaState       *state,
                            ShellLuaWorkerStats *stats)
{
  Group *group;

  g_return_if_fail (stats != NULL);

  memset (stats, 0, sizeof *stats);
  if (state == NULL)
    return;

  group = g_object_get_data (G_OBJECT (state), GROUP_KEY);
  if (group == NULL)
    return;

  g_mutex_lock (&group->lock);
  stats->messages = group->messages;
  stats->total_us = group->total_us;
  stats->longest_us = group->longest_us;
  g_mutex_unlock (&group->lock);
  stats->live = (guint64) g_atomic_int_get (&group->live);
}

/* ------------------------------------------------------------------------ */
/* Queue items                                                               */
/* ------------------------------------------------------------------------ */

typedef enum {
  ITEM_MESSAGE,   /* a value, either direction */
  ITEM_LOG,       /* a line of shell.log or print () of the worker */
  ITEM_ENDED,     /* the worker is over: code and message for on_error */
} ItemKind;

typedef struct {
  ItemKind        kind;
  GVariant       *value;
  GLogLevelFlags  level;
  char           *text;
  const char     *code;   /* static */
} Item;

static Item *
item_new_message (GVariant *value)
{
  Item *item = g_new0 (Item, 1);

  item->kind = ITEM_MESSAGE;
  item->value = g_variant_ref (value);
  return item;
}

static Item *
item_new_log (GLogLevelFlags  level,
              const char     *text)
{
  Item *item = g_new0 (Item, 1);

  item->kind = ITEM_LOG;
  item->level = level;
  item->text = g_strdup (text);
  return item;
}

static Item *
item_new_ended (const char *code,
                const char *message)
{
  Item *item = g_new0 (Item, 1);

  item->kind = ITEM_ENDED;
  item->code = code;
  item->text = g_strdup (message);
  return item;
}

static void
item_free (gpointer data)
{
  Item *item = data;

  g_clear_pointer (&item->value, g_variant_unref);
  g_free (item->text);
  g_free (item);
}

static void
queue_clear (GQueue *queue)
{
  Item *item;

  while ((item = g_queue_pop_head (queue)) != NULL)
    item_free (item);
}

/* ------------------------------------------------------------------------ */
/* ShellLuaWorker                                                            */
/* ------------------------------------------------------------------------ */

#define SHELL_TYPE_LUA_WORKER (shell_lua_worker_get_type ())
G_DECLARE_FINAL_TYPE (ShellLuaWorker, shell_lua_worker, SHELL, LUA_WORKER, GObject)

struct _ShellLuaWorker
{
  GObject parent_instance;

  /* Main thread */
  ShellLuaState *state;        /* the extension's state (referenced); NULL once let go */
  Group         *group;
  int            message_ref;  /* on_message, in the extension's state */
  int            error_ref;    /* on_error */
  gboolean       destroyed;    /* the destroy rule ran: the handle is dead */
  gboolean       terminating;  /* terminate (): on_error "terminated" is on its way */
  gboolean       finished;     /* nothing of the main side runs any more */

  /* What the thread needs; written before it starts, read only after */
  char          *id;           /* "<uuid>/worker-<n>", the id of the worker's state */
  char          *module;
  char          *uuid;         /* NULL: the state has no extension */
  char          *dir;
  char         **capabilities;
  char          *metadata;     /* JSON */
  GPtrArray     *roots;        /* GFile: the require () roots */
  gsize          max_memory;

  /* Shared, under @lock */
  GMutex         lock;
  GCond          inbox_cond;   /* the thread waits for a message or the stop */
  GCond          outbox_cond;  /* the thread waits for room */
  GQueue         inbox;        /* Item, toward the worker */
  GQueue         outbox;       /* Item, toward the main thread */
  gboolean       stop;
  ShellLuaState *worker_state; /* while the thread runs its state (not referenced here) */
  GSource       *source;       /* the drain, while the main side listens */
};

G_DEFINE_FINAL_TYPE (ShellLuaWorker, shell_lua_worker, G_TYPE_OBJECT)

static void
shell_lua_worker_finalize (GObject *object)
{
  ShellLuaWorker *worker = SHELL_LUA_WORKER (object);

  g_assert (worker->state == NULL);
  g_assert (worker->source == NULL);

  queue_clear (&worker->inbox);
  queue_clear (&worker->outbox);
  g_mutex_clear (&worker->lock);
  g_cond_clear (&worker->inbox_cond);
  g_cond_clear (&worker->outbox_cond);
  g_clear_pointer (&worker->group, group_unref);
  g_clear_pointer (&worker->roots, g_ptr_array_unref);
  g_strfreev (worker->capabilities);
  g_free (worker->id);
  g_free (worker->module);
  g_free (worker->uuid);
  g_free (worker->dir);
  g_free (worker->metadata);

  G_OBJECT_CLASS (shell_lua_worker_parent_class)->finalize (object);
}

static void
shell_lua_worker_class_init (ShellLuaWorkerClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = shell_lua_worker_finalize;
}

static void
shell_lua_worker_init (ShellLuaWorker *worker)
{
  worker->message_ref = LUA_NOREF;
  worker->error_ref = LUA_NOREF;
  g_mutex_init (&worker->lock);
  g_cond_init (&worker->inbox_cond);
  g_cond_init (&worker->outbox_cond);
  g_queue_init (&worker->inbox);
  g_queue_init (&worker->outbox);
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

/* Wakes the drain. Called with the lock held (see the notes at the top). */
static void
wake_main (ShellLuaWorker *worker)
{
  if (worker->source != NULL)
    g_source_set_ready_time (worker->source, 0);
}

/* Lets go of the drain. Main thread. */
static void
drop_source (ShellLuaWorker *worker)
{
  GSource *source;

  g_mutex_lock (&worker->lock);
  source = worker->source;
  worker->source = NULL;
  g_mutex_unlock (&worker->lock);

  if (source != NULL)
    {
      g_source_destroy (source);
      g_source_unref (source);
    }
}

/* Stops the thread and forgets the queues. Main thread. */
static void
stop_thread (ShellLuaWorker *worker)
{
  g_mutex_lock (&worker->lock);
  worker->stop = TRUE;
  queue_clear (&worker->inbox);
  queue_clear (&worker->outbox);
  if (worker->worker_state != NULL)
    shell_lua_state_request_stop (worker->worker_state);
  g_cond_broadcast (&worker->inbox_cond);
  g_cond_broadcast (&worker->outbox_cond);
  g_mutex_unlock (&worker->lock);
}

/* Nothing of the main side runs any more: the callbacks and the state are
 * released, the drain is gone. */
static void
let_go (ShellLuaWorker *worker)
{
  ShellLuaState *state = worker->state;

  worker->finished = TRUE;
  worker->state = NULL;
  release_ref (state, &worker->message_ref);
  release_ref (state, &worker->error_ref);
  drop_source (worker);
  g_clear_object (&state);
}

/* The destroy rule (destroy (), disable, the end of the worker): the handle
 * dies, the thread is stopped, on_message is dropped. terminate () keeps
 * on_error (and the state and the drain for it) until it has run. */
static void
worker_destroy_op (GObject *object)
{
  ShellLuaWorker *worker = SHELL_LUA_WORKER (object);

  if (worker->destroyed)
    return;
  worker->destroyed = TRUE;

  stop_thread (worker);
  release_ref (worker->state, &worker->message_ref);
  if (!worker->terminating && !worker->finished)
    let_go (worker);
}

static const ShellLuaOwnOps worker_ops = { .destroy = worker_destroy_op };

/* ------------------------------------------------------------------------ */
/* Main thread: delivering the outbox                                        */
/* ------------------------------------------------------------------------ */

typedef struct {
  const char *message;
  const char *code;
} ErrorArgs;

static void
push_error_args (lua_State *L,
                 gpointer   data)
{
  ErrorArgs *args = data;

  lua_createtable (L, 0, 2);
  lua_pushstring (L, args->message);
  lua_setfield (L, -2, "message");
  lua_pushstring (L, args->code);
  lua_setfield (L, -2, "code");
}

/* Runs inside the protected call of on_message, so the conversion is timed
 * with the callback and a failure of it is an error of the callback. */
static void
push_message_arg (lua_State *L,
                  gpointer   data)
{
  shell_lua_push_variant (L, data);
}

/* The worker is over, with the @code of on_error. Hands the slot of owned
 * objects back (the handle dies), reports the error if it is one, runs
 * on_error, lets go. */
static void
worker_finish (ShellLuaWorker *worker,
               const char     *code,
               const char     *message)
{
  g_autoptr (ShellLuaState) state = NULL;
  gboolean counts = strcmp (code, "terminated") != 0;
  int error_ref;

  if (worker->finished)
    return;

  g_object_ref (worker);
  state = worker->state != NULL ? g_object_ref (worker->state) : NULL;
  error_ref = worker->error_ref;
  worker->error_ref = LUA_NOREF;

  if (!worker->destroyed)
    {
      if (state != NULL && shell_lua_state_is_alive (state))
        shell_lua_destroy_object (shell_lua_state_get_L (state), G_OBJECT (worker));
      else
        worker_destroy_op (G_OBJECT (worker));
    }

  if (state != NULL && shell_lua_state_is_alive (state) && !shell_lua_objects_is_blocked (state))
    {
      if (counts)
        {
          g_autofree char *report = g_strdup_printf ("%s: %s",
                                                     strrchr (worker->id, '/') != NULL
                                                     ? strrchr (worker->id, '/') + 1 : worker->id,
                                                     message);

          shell_lua_state_report_error (state,
                                        strcmp (code, "memory") == 0
                                        ? SHELL_LUA_ERROR_MEMORY : SHELL_LUA_ERROR_RUNTIME,
                                        report);
        }

      if (error_ref != LUA_NOREF)
        {
          ErrorArgs args = { message, code };

          shell_lua_emit_ref (state, NULL, error_ref, 1, push_error_args, &args);
        }
    }

  release_ref (state, &error_ref);
  let_go (worker);
  g_object_unref (worker);
}

typedef struct {
  GSource         source;
  ShellLuaWorker *worker;
} DrainSource;

/* One item per dispatch; the ready time is set under the lock, together
 * with the thread's wakeups. */
static gboolean
drain_dispatch (GSource     *source,
                GSourceFunc  callback,
                gpointer     user_data)
{
  ShellLuaWorker *worker = ((DrainSource *) source)->worker;
  Item *item;

  g_mutex_lock (&worker->lock);
  item = g_queue_pop_head (&worker->outbox);
  if (item != NULL)
    g_cond_signal (&worker->outbox_cond);
  g_source_set_ready_time (source, g_queue_is_empty (&worker->outbox) ? -1 : 0);
  g_mutex_unlock (&worker->lock);

  if (item == NULL)
    return G_SOURCE_CONTINUE;

  g_object_ref (worker);
  switch (item->kind)
    {
    case ITEM_MESSAGE:
      if (!worker->finished && worker->message_ref != LUA_NOREF && worker->state != NULL)
        shell_lua_emit_ref (worker->state, NULL, worker->message_ref, 1, push_message_arg, item->value);
      break;
    case ITEM_LOG:
      if (!worker->finished)
        shell_lua_write_extension_log (worker->uuid != NULL ? worker->uuid : worker->id,
                                       item->level, item->text);
      break;
    case ITEM_ENDED:
      worker_finish (worker, item->code, item->text);
      break;
    }
  item_free (item);
  g_object_unref (worker);

  return G_SOURCE_CONTINUE;
}

static void
drain_finalize (GSource *source)
{
  g_clear_object (&((DrainSource *) source)->worker);
}

static GSourceFuncs drain_funcs = {
  .dispatch = drain_dispatch,
  .finalize = drain_finalize,
};

static GSource *
drain_source_new (ShellLuaWorker *worker)
{
  GSource *source = g_source_new (&drain_funcs, sizeof (DrainSource));

  ((DrainSource *) source)->worker = g_object_ref (worker);
  g_source_set_static_name (source, "[shell-lua] worker outbox");
  g_source_set_priority (source, G_PRIORITY_DEFAULT);
  g_source_set_ready_time (source, -1);
  return source;
}

/* ------------------------------------------------------------------------ */
/* The thread                                                                */
/* ------------------------------------------------------------------------ */

static gint64
thread_cpu_us (void)
{
  struct timespec ts;

  if (clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
    return 0;
  return (gint64) ts.tv_sec * G_USEC_PER_SEC + ts.tv_nsec / 1000;
}

/* Queues @item toward the main thread, waiting for room; raises in @L when
 * the worker was stopped meanwhile (the item is then freed). The caller
 * holds nothing else: the raise would leak it. Thread. */
static void
outbox_push (ShellLuaWorker *worker,
             lua_State      *L,
             Item           *item)
{
  gboolean stopped;

  g_mutex_lock (&worker->lock);
  while (g_queue_get_length (&worker->outbox) >= QUEUE_LIMIT && !worker->stop)
    g_cond_wait (&worker->outbox_cond, &worker->lock);
  stopped = worker->stop;
  if (!stopped)
    {
      g_queue_push_tail (&worker->outbox, item);
      wake_main (worker);
    }
  g_mutex_unlock (&worker->lock);

  if (stopped)
    {
      item_free (item);
      luaL_error (L, STOPPED_MESSAGE);
    }
}

/* The end of the worker: never waits (the main side may be gone). */
static void
outbox_push_ended (ShellLuaWorker *worker,
                   const char     *code,
                   const char     *message)
{
  g_mutex_lock (&worker->lock);
  g_queue_push_tail (&worker->outbox, item_new_ended (code, message));
  wake_main (worker);
  g_mutex_unlock (&worker->lock);
}

/* print () and shell.log of the worker (ShellLuaMessageSink) */
static void
worker_sink (lua_State      *L,
             GLogLevelFlags  level,
             const char     *message,
             gpointer        data)
{
  outbox_push (data, L, item_new_log (level, message));
}

/* Converts the value at @idx to a message or raises. */
static GVariant *
to_message (lua_State *L,
            int        idx)
{
  GVariant *value = shell_lua_to_variant_full (L, idx, NULL,
                                               SHELL_LUA_VARIANT_BYTES_AS_AY | SHELL_LUA_VARIANT_STRICT_KEYS,
                                               max_message_values);

  if (g_variant_get_size (value) > MAX_MESSAGE_BYTES)
    {
      g_variant_unref (value);
      luaL_error (L, "message is too large (limit %d bytes)", (int) MAX_MESSAGE_BYTES);
      return NULL;
    }
  return value;
}

/* shell.worker.post (value), inside a worker */
static int
l_post_to_main (lua_State *L)
{
  ShellLuaState *state = shell_lua_state_from_L (L);
  gpointer owner = state != NULL ? shell_lua_state_get_owner (state) : NULL;
  GVariant *value;
  Item *item;

  if (!SHELL_IS_LUA_WORKER (owner))
    return luaL_error (L, "shell.worker.post() is only available inside a worker");
  if (lua_gettop (L) != 1)
    return luaL_error (L, "shell.worker.post() takes exactly one value");

  value = to_message (L, 1);
  item = item_new_message (value);
  g_variant_unref (value);
  outbox_push (owner, L, item);
  return 0;
}

typedef struct {
  GVariant *value;
  int       handler_ref;
  gint64    cpu_start;   /* thread CPU time when the handler began */
  gboolean  accounted;
} HandlerArgs;

static void
account_handler (ShellLuaWorker *worker,
                 HandlerArgs    *handle)
{
  group_account (worker->group, thread_cpu_us () - handle->cpu_start);
  handle->accounted = TRUE;
}

/* Runs under the protected call of the worker's state: on_message (value),
 * and its result is posted. The handler is accounted before its result is
 * posted, so the extension never gets an answer that the figures do not
 * count yet. */
static int
run_handler (lua_State *L)
{
  HandlerArgs *handle = lua_touserdata (L, 1);
  ShellLuaState *state = shell_lua_state_from_L (L);
  ShellLuaWorker *worker = shell_lua_state_get_owner (state);
  GVariant *result;
  Item *item;

  lua_settop (L, 0);
  lua_rawgeti (L, LUA_REGISTRYINDEX, handle->handler_ref);
  shell_lua_push_variant (L, handle->value);
  shell_lua_state_note_callee (L, 1);
  lua_call (L, 1, 1);
  account_handler (worker, handle);

  if (lua_isnil (L, 1))
    return 0;

  result = to_message (L, 1);
  item = item_new_message (result);
  g_variant_unref (result);
  outbox_push (worker, L, item);
  return 0;
}

/* shell.extension of the worker: a read-only copy of what the extension
 * side shows, built from the strings the main thread prepared. Runs under
 * a protected call (the JSON may be deep). arg 1: the worker. */
static int
install_extension_table (lua_State *L)
{
  ShellLuaWorker *worker = lua_touserdata (L, 1);
  g_autoptr (JsonNode) metadata = NULL;
  int i;

  lua_settop (L, 0);
  lua_getglobal (L, "shell");

  lua_createtable (L, 0, 4);
  lua_pushstring (L, worker->uuid);
  lua_setfield (L, -2, "uuid");
  lua_pushstring (L, worker->dir != NULL ? worker->dir : "");
  lua_setfield (L, -2, "dir");

  metadata = worker->metadata != NULL ? json_from_string (worker->metadata, NULL) : NULL;
  if (metadata != NULL)
    shell_lua_push_json_node (L, metadata);
  else
    {
      lua_newtable (L);
      shell_lua_make_readonly (L);
    }
  lua_setfield (L, -2, "metadata");

  lua_newtable (L);
  for (i = 0; worker->capabilities != NULL && worker->capabilities[i] != NULL; i++)
    {
      lua_pushstring (L, worker->capabilities[i]);
      lua_rawseti (L, -2, i + 1);
    }
  shell_lua_make_readonly (L);
  lua_setfield (L, -2, "capabilities");

  shell_lua_make_readonly (L);
  lua_setfield (L, -2, "extension");
  return 0;
}

/* Loads the module: `require (name)`, which validates the name and gives
 * the usual messages. arg 1: the module name. Leaves the result. */
static int
load_module (lua_State *L)
{
  lua_settop (L, 1);
  lua_getglobal (L, "require");
  lua_insert (L, 1);
  lua_call (L, 1, 1);
  return 1;
}

static const char *
code_for_error (GError *error)
{
  if (g_error_matches (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_STOPPED))
    return "terminated";
  if (g_error_matches (error, SHELL_LUA_ERROR, SHELL_LUA_ERROR_MEMORY))
    return "memory";
  return "error";
}

/* Creates the state, loads the module, handles messages until the stop or
 * an error, closes the state. Returns with the code and message of the end
 * in *code / *message (NULL message: a normal stop). */
static void
worker_run (ShellLuaWorker  *worker,
            const char     **code,
            char           **message)
{
  ShellLuaLimits limits = { 0, 0, worker->max_memory };
  g_autoptr (ShellLuaState) state = NULL;
  g_autoptr (GError) error = NULL;
  lua_State *L;
  int handler_ref = LUA_NOREF;
  guint i;

  *code = "terminated";
  *message = NULL;

  state = shell_lua_state_new (worker->id, SHELL_LUA_STATE_WORKER, &limits);
  if (!shell_lua_state_is_alive (state))
    {
      *code = "load";
      *message = g_strdup ("could not create the Lua state of the worker");
      return;
    }

  shell_lua_state_set_owner (state, worker);
  shell_lua_state_set_message_sink (state, worker_sink, worker);
  for (i = 0; i < worker->roots->len; i++)
    shell_lua_state_add_search_root (state, g_ptr_array_index (worker->roots, i));
  L = shell_lua_state_get_L (state);

  /* From here on the main thread stops the state through worker_state; a
   * stop that came before this point is applied now, so that a module that
   * never returns from its load is stopped too */
  g_mutex_lock (&worker->lock);
  if (worker->stop)
    shell_lua_state_request_stop (state);
  else
    worker->worker_state = state;
  g_mutex_unlock (&worker->lock);

  if (worker->uuid != NULL)
    {
      lua_pushcfunction (L, install_extension_table);
      lua_pushlightuserdata (L, worker);
      if (!shell_lua_state_pcall (state, 1, 0, &error))
        {
          *code = code_for_error (error);
          if (strcmp (*code, "error") == 0)
            *code = "load";
          *message = g_strdup (error->message);
          goto out;
        }
    }

  lua_pushcfunction (L, load_module);
  lua_pushstring (L, worker->module);
  if (!shell_lua_state_pcall (state, 1, 1, &error))
    {
      *code = code_for_error (error);
      if (strcmp (*code, "error") == 0)
        *code = "load";
      *message = g_strdup (error->message);
      goto out;
    }

  /* raw: a metatable on the table must not run code outside a protected call */
  if (lua_type (L, -1) == LUA_TTABLE)
    {
      lua_pushliteral (L, "on_message");
      lua_rawget (L, -2);
      if (lua_isfunction (L, -1))
        handler_ref = shell_lua_state_ref (state);
      else
        lua_pop (L, 1);
    }
  lua_pop (L, 1);

  if (handler_ref == LUA_NOREF)
    {
      *code = "load";
      *message = g_strdup_printf ("module '%s' must return a table with an on_message function",
                                  worker->module);
      goto out;
    }

  for (;;)
    {
      HandlerArgs handle = { NULL, handler_ref, 0, FALSE };
      Item *item;
      gboolean ok;

      g_mutex_lock (&worker->lock);
      while (g_queue_is_empty (&worker->inbox) && !worker->stop)
        g_cond_wait (&worker->inbox_cond, &worker->lock);
      item = worker->stop ? NULL : g_queue_pop_head (&worker->inbox);
      g_mutex_unlock (&worker->lock);

      if (item == NULL)
        break;

      handle.value = item->value;
      handle.cpu_start = thread_cpu_us ();
      lua_pushcfunction (L, run_handler);
      lua_pushlightuserdata (L, &handle);
      ok = shell_lua_state_pcall (state, 1, 0, &error);
      if (!handle.accounted)
        account_handler (worker, &handle);
      item_free (item);

      if (!ok)
        {
          *code = code_for_error (error);
          *message = g_strdup (error->message);
          break;
        }
    }

out:
  g_mutex_lock (&worker->lock);
  worker->worker_state = NULL;
  g_mutex_unlock (&worker->lock);

  if (handler_ref != LUA_NOREF)
    shell_lua_state_unref (state, handler_ref);
  shell_lua_state_destroy (state);
}

static gpointer
worker_thread (gpointer data)
{
  ShellLuaWorker *worker = data;
  const char *code;
  char *message;

  /* Below the compositor and the user's applications. Linux: the nice value
   * of the calling thread; failing is harmless. */
  (void) setpriority (PRIO_PROCESS, 0, WORKER_NICE);

  worker_run (worker, &code, &message);

  outbox_push_ended (worker, code, message != NULL ? message : "stopped");
  g_free (message);

  g_atomic_int_add (&worker->group->live, -1);
  release_slot ();
  g_object_unref (worker);
  return NULL;
}

/* ------------------------------------------------------------------------ */
/* Lua side: shell.Worker                                                    */
/* ------------------------------------------------------------------------ */

static int
l_worker_post (lua_State *L)
{
  ShellLuaWorker *worker = SHELL_LUA_WORKER (shell_lua_handle_check (L, 1, "shell.Worker"));
  GVariant *value;
  gboolean full;

  if (lua_gettop (L) != 2)
    return luaL_error (L, "post() takes exactly one value");

  value = to_message (L, 2);

  g_mutex_lock (&worker->lock);
  full = g_queue_get_length (&worker->inbox) >= QUEUE_LIMIT;
  if (!full)
    {
      g_queue_push_tail (&worker->inbox, item_new_message (value));
      g_cond_signal (&worker->inbox_cond);
    }
  g_mutex_unlock (&worker->lock);
  g_variant_unref (value);

  if (full)
    return luaL_error (L, "too many queued messages (limit %d)", QUEUE_LIMIT);
  return 0;
}

static int
l_worker_terminate (lua_State *L)
{
  GObject *object = shell_lua_handle_get (L, 1, "shell.Worker");
  ShellLuaWorker *worker;

  if (object == NULL)
    return 0;
  worker = SHELL_LUA_WORKER (object);
  if (worker->finished || worker->destroyed)
    return 0;

  /* The handle dies now; on_error runs later, from the drain */
  g_object_ref (worker);
  worker->terminating = TRUE;
  shell_lua_destroy_object (L, object);

  g_mutex_lock (&worker->lock);
  g_queue_push_tail (&worker->outbox, item_new_ended ("terminated", "the worker was terminated"));
  wake_main (worker);
  g_mutex_unlock (&worker->lock);

  g_object_unref (worker);
  return 0;
}

static void
worker_get_queued (lua_State *L,
                   GObject   *object)
{
  ShellLuaWorker *worker = SHELL_LUA_WORKER (object);
  guint n;

  g_mutex_lock (&worker->lock);
  n = g_queue_get_length (&worker->inbox);
  g_mutex_unlock (&worker->lock);
  lua_pushinteger (L, n);
}

static const luaL_Reg worker_methods[] = {
  { "post", l_worker_post },
  { NULL, NULL }
};

static const ShellLuaHandleMethod worker_methods_ex[] = {
  { "terminate", l_worker_terminate,              SHELL_LUA_METHOD_DEAD_OK },
  { "destroy",   shell_lua_handle_destroy_method, SHELL_LUA_METHOD_DEAD_OK },
  { NULL }
};

static const ShellLuaHandleProperty worker_properties[] = {
  { NULL }
};

static const ShellLuaHandleVirtual worker_virtuals[] = {
  { "queued", worker_get_queued, NULL },
  { NULL }
};

static ShellLuaHandleType worker_type = {
  .name = "shell.Worker",
  .methods = worker_methods,
  .methods_ex = worker_methods_ex,
  .properties = worker_properties,
  .virtuals = worker_virtuals,
};

/* ------------------------------------------------------------------------ */
/* Lua side: shell.worker.start                                              */
/* ------------------------------------------------------------------------ */

static const char * const start_fields[] = {
  "module", "max_memory", "on_message", "on_error", NULL
};

static gboolean
is_callable (lua_State *L,
             int        idx)
{
  if (lua_type (L, idx) == LUA_TFUNCTION)
    return TRUE;
  if (luaL_getmetafield (L, idx, "__call") == LUA_TNIL)
    return FALSE;
  lua_pop (L, 1);
  return TRUE;
}

static char *
metadata_to_string (JsonObject *metadata)
{
  g_autoptr (JsonNode) node = json_node_new (JSON_NODE_OBJECT);

  json_node_set_object (node, metadata);
  return json_to_string (node, FALSE);
}

static int
l_worker_start (lua_State *L)
{
  ShellLuaState *state = shell_lua_state_from_L (L);
  ShellLuaExtension *extension;
  ShellLuaWorker *worker;
  Group *group;
  GThread *thread;
  GError *error = NULL;
  const char *module;
  lua_Integer max_memory;
  int message_idx, error_idx;

  shell_lua_require_capability (L, "worker", "shell.worker.start()");
  shell_lua_check_fields (L, 1, start_fields);

  /* 1. read and validate: nothing is allocated yet */
  module = shell_lua_opt_string (L, 1, "module", NULL);
  if (module == NULL)
    return luaL_error (L, "missing required field 'module' (a string)");
  if (!shell_lua_module_name_is_valid (module))
    return luaL_error (L, "invalid module name '%s'", module);

  max_memory = shell_lua_opt_integer (L, 1, "max_memory", (lua_Integer) DEFAULT_MAX_MEMORY);
  if (max_memory < (lua_Integer) MIN_MAX_MEMORY || max_memory > (lua_Integer) MAX_MAX_MEMORY)
    return luaL_error (L, "field 'max_memory' must be between %d and %d",
                       (int) MIN_MAX_MEMORY, (int) MAX_MAX_MEMORY);

  lua_getfield (L, 1, "on_message");
  message_idx = lua_gettop (L);
  lua_getfield (L, 1, "on_error");
  error_idx = lua_gettop (L);
  if (!lua_isnil (L, message_idx) && !is_callable (L, message_idx))
    return luaL_error (L, "field 'on_message' must be a function, got %s", luaL_typename (L, message_idx));
  if (!lua_isnil (L, error_idx) && !is_callable (L, error_idx))
    return luaL_error (L, "field 'on_error' must be a function, got %s", luaL_typename (L, error_idx));

  shell_lua_check_owned_capacity (L);

  group = group_for_state (state);
  if (g_atomic_int_get (&group->live) >= MAX_WORKERS_PER_STATE)
    return luaL_error (L, "too many workers (limit %d)", MAX_WORKERS_PER_STATE);

  /* the shell-wide limit depends on the other extensions: environmental */
  if (!reserve_slot ())
    {
      lua_pushnil (L);
      lua_pushfstring (L, "the shell has no room for another worker (limit %d)",
                       (int) shell_lua_worker_get_limit ());
      return 2;
    }

  /* 2. make and start: no Lua code runs until the worker is owned */
  g_atomic_int_inc (&group->live);
  group->next_id++;

  extension = shell_lua_extension_from_L (L);
  worker = g_object_new (SHELL_TYPE_LUA_WORKER, NULL);
  worker->state = g_object_ref (state);
  worker->group = group_ref (group);
  worker->module = g_strdup (module);
  worker->max_memory = (gsize) max_memory;
  worker->roots = shell_lua_state_dup_search_roots (state);
  if (extension != NULL)
    {
      worker->uuid = g_strdup (shell_lua_extension_get_uuid (extension));
      worker->dir = g_file_get_path (shell_lua_extension_get_dir (extension));
      worker->capabilities = g_strdupv ((char **) shell_lua_extension_get_capabilities (extension));
      worker->metadata = metadata_to_string (shell_lua_extension_get_metadata (extension));
    }
  worker->id = g_strdup_printf ("%s/worker-%u",
                                worker->uuid != NULL ? worker->uuid : shell_lua_state_get_id (state),
                                group->next_id);

  if (!lua_isnil (L, message_idx))
    {
      lua_pushvalue (L, message_idx);
      worker->message_ref = luaL_ref (L, LUA_REGISTRYINDEX);
    }
  if (!lua_isnil (L, error_idx))
    {
      lua_pushvalue (L, error_idx);
      worker->error_ref = luaL_ref (L, LUA_REGISTRYINDEX);
    }

  shell_lua_own_object (L, G_OBJECT (worker), &worker_ops);
  g_object_unref (worker);   /* the owner holds it now */

  worker->source = drain_source_new (worker);
  g_source_attach (worker->source, g_main_context_default ());

  thread = g_thread_try_new ("lua-worker", worker_thread, g_object_ref (worker), &error);
  if (thread == NULL)
    {
      g_object_unref (worker);
      g_atomic_int_add (&group->live, -1);
      release_slot ();
      shell_lua_destroy_object (L, G_OBJECT (worker));
      lua_pushnil (L);
      lua_pushstring (L, error->message);
      g_error_free (error);
      return 2;
    }
  g_thread_unref (thread);

  shell_lua_handle_push (L, "shell.Worker", G_OBJECT (worker));
  return 1;
}

void
shell_lua_api_worker_register (ShellLuaState *state,
                               lua_State     *L)
{
  int shell = lua_absindex (L, -1);

  luaL_checkstack (L, 4, "shell worker api");

  lua_newtable (L);
  if (shell_lua_state_get_kind (state) == SHELL_LUA_STATE_WORKER)
    {
      /* inside a worker: post, nothing else */
      lua_pushcfunction (L, l_post_to_main);
      lua_setfield (L, -2, "post");
    }
  else
    {
      worker_type.gtype = SHELL_TYPE_LUA_WORKER;
      shell_lua_handle_register_type (L, &worker_type);

      lua_pushcfunction (L, l_worker_start);
      lua_setfield (L, -2, "start");
      lua_pushinteger (L, shell_lua_worker_get_limit ());
      lua_setfield (L, -2, "limit");
    }
  lua_setfield (L, shell, "worker");
}
