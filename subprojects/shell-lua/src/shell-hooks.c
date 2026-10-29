/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellHookRegistry implementation. See shell-hooks.h for the contract.
 *
 * Implementation notes:
 *  - Listeners are reference counted. The hook point's arrays own one
 *    reference; every emission takes one more per listener in its snapshot.
 *    disconnect() marks the listener removed, drops it from its array and the
 *    id table and releases the array's reference; the GDestroyNotify runs when
 *    the last reference goes away, i.e. immediately if no emission holds the
 *    listener, otherwise when that emission finishes. It runs exactly once.
 *  - A listener removed while an emission is in progress is skipped if its
 *    turn has not yet come; listeners connected during an emission are not
 *    part of the snapshot and so are not called by it.
 *  - Payloads passed to emit_* are consumed if floating (g_variant_ref_sink),
 *    following the usual GLib convention.
 *  - Replay (shell_hook_registry_set_replay): a listener of the "after" phase
 *    of a hook with a replay is "replay pending" from its connection until an
 *    idle source ran the replay function and delivered its payloads to it.
 *    Emissions skip a pending listener (the replay reflects the state they
 *    describe). The idle source belongs to the listener (it is removed when
 *    the listener is freed), so a registry that goes away with a pending
 *    replay leaves nothing behind.
 */

#include "shell-hooks.h"

#include <string.h>

G_DEFINE_ENUM_TYPE (ShellHookVerdict, shell_hook_verdict,
                    G_DEFINE_ENUM_VALUE (SHELL_HOOK_VERDICT_CONTINUE, "continue"),
                    G_DEFINE_ENUM_VALUE (SHELL_HOOK_VERDICT_CANCEL, "cancel"))

G_DEFINE_ENUM_TYPE (ShellHookPhase, shell_hook_phase,
                    G_DEFINE_ENUM_VALUE (SHELL_HOOK_PHASE_BEFORE, "before"),
                    G_DEFINE_ENUM_VALUE (SHELL_HOOK_PHASE_AFTER, "after"))

#define N_PHASES 2

typedef struct _HookPoint HookPoint;

typedef struct
{
  guint              id;
  ShellHookListener  fn;
  gpointer           user_data;
  GDestroyNotify     destroy;
  HookPoint         *point;
  ShellHookPhase     phase;
  gboolean           removed;
  gboolean           replay_pending;
  guint              replay_source;
  int                ref_count;
} Listener;

struct _HookPoint
{
  char       *name;
  char       *schema;
  char       *doc;
  gboolean    cancellable;
  GPtrArray  *listeners[N_PHASES]; /* Listener *, each holding one ref */
  guint       emission_depth;
  ShellHookRegistry   *registry;     /* not owning: the registry owns the point */
  ShellHookReplayFunc  replay;
  gpointer             replay_data;
};

struct _ShellHookRegistry
{
  GObject      parent_instance;

  GHashTable  *points;  /* char *name -> HookPoint * */
  GHashTable  *by_id;   /* guint id -> Listener * (not owning) */
  guint        next_id;
};

enum
{
  SIGNAL_DECLARED,
  SIGNAL_EMITTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (ShellHookRegistry, shell_hook_registry, G_TYPE_OBJECT)

/* ---- listeners -------------------------------------------------------- */

static Listener *
listener_ref (Listener *l)
{
  l->ref_count++;
  return l;
}

static void
listener_unref (Listener *l)
{
  if (--l->ref_count > 0)
    return;

  if (l->replay_source != 0)
    g_source_remove (l->replay_source);
  if (l->destroy)
    l->destroy (l->user_data);
  g_free (l);
}

/* ---- hook points ------------------------------------------------------ */

static void
hook_point_free (HookPoint *hp)
{
  for (int i = 0; i < N_PHASES; i++)
    {
      GPtrArray *a = hp->listeners[i];

      while (a->len > 0)
        {
          Listener *l = g_ptr_array_index (a, a->len - 1);

          g_ptr_array_set_size (a, a->len - 1);
          l->removed = TRUE;
          listener_unref (l);
        }
      g_ptr_array_unref (a);
    }
  g_free (hp->name);
  g_free (hp->schema);
  g_free (hp->doc);
  g_free (hp);
}

/* ^[a-z][a-z0-9-]*(\.[a-z][a-z0-9-]*)+$: hyphenated segments on both sides
 * of the dot (`quick-settings.opened`, `window.title-changed`) */
static gboolean
hook_name_is_valid (const char *name)
{
  const char *p = name;
  int segments = 0;

  if (name == NULL)
    return FALSE;

  while (TRUE)
    {
      if (!(*p >= 'a' && *p <= 'z'))
        return FALSE;
      p++;
      while ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-')
        p++;
      segments++;

      if (*p == '\0')
        return segments >= 2;
      if (*p != '.')
        return FALSE;
      p++;
    }
}

static HookPoint *
lookup_point (ShellHookRegistry *self,
              const char        *name)
{
  return name ? g_hash_table_lookup (self->points, name) : NULL;
}

/* ---- GObject ---------------------------------------------------------- */

static void
shell_hook_registry_finalize (GObject *object)
{
  ShellHookRegistry *self = SHELL_HOOK_REGISTRY (object);

  g_clear_pointer (&self->by_id, g_hash_table_unref);
  g_clear_pointer (&self->points, g_hash_table_unref);

  G_OBJECT_CLASS (shell_hook_registry_parent_class)->finalize (object);
}

static void
shell_hook_registry_class_init (ShellHookRegistryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = shell_hook_registry_finalize;

  /**
   * ShellHookRegistry::declared:
   * @self: the registry
   * @name: the name of the newly declared hook point
   */
  signals[SIGNAL_DECLARED] =
    g_signal_new ("declared",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);

  /**
   * ShellHookRegistry::emitted:
   * @self: the registry
   * @name: the hook name
   * @phase: the phase being emitted
   * @payload: the a{sv} payload
   *
   * Emitted at the start of every emission phase, before listeners run.
   * Intended for tracing (Looking Glass).
   */
  signals[SIGNAL_EMITTED] =
    g_signal_new ("emitted",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 3,
                  G_TYPE_STRING,
                  SHELL_TYPE_HOOK_PHASE,
                  G_TYPE_VARIANT);
}

static void
shell_hook_registry_init (ShellHookRegistry *self)
{
  self->points = g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
                                        (GDestroyNotify) hook_point_free);
  self->by_id = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->next_id = 1;
}

/**
 * shell_hook_registry_get_default:
 *
 * Returns: (transfer none): the process-wide hook registry
 */
ShellHookRegistry *
shell_hook_registry_get_default (void)
{
  static gsize default_registry = 0;

  if (g_once_init_enter (&default_registry))
    {
      gpointer obj = g_object_new (SHELL_TYPE_HOOK_REGISTRY, NULL);
      g_once_init_leave (&default_registry, (gsize) obj);
    }

  return (ShellHookRegistry *) default_registry;
}

/* ---- declaration ------------------------------------------------------ */

/**
 * shell_hook_registry_declare:
 * @self: the registry
 * @name: hook name, "<subject>.<event>"
 * @payload_schema: human readable description of the payload keys
 * @cancellable: whether the BEFORE phase exists and may cancel
 * @doc: (nullable): human readable description
 *
 * Declares a hook point. Idempotent; redeclaring with a different schema or
 * cancellable flag is a programmer error.
 */
void
shell_hook_registry_declare (ShellHookRegistry *self,
                             const char        *name,
                             const char        *payload_schema,
                             gboolean           cancellable,
                             const char        *doc)
{
  HookPoint *hp;
  const char *schema = payload_schema ? payload_schema : "";

  g_return_if_fail (SHELL_IS_HOOK_REGISTRY (self));

  if (!hook_name_is_valid (name))
    {
      g_critical ("Invalid hook name '%s' (expected <subject>.<event>, "
                  "lower-case, e.g. \"window.focused\")",
                  name ? name : "(null)");
      return;
    }

  cancellable = !!cancellable;
  hp = lookup_point (self, name);
  if (hp != NULL)
    {
      if (hp->cancellable != cancellable || g_strcmp0 (hp->schema, schema) != 0)
        g_critical ("Hook '%s' redeclared with a different %s",
                    name,
                    hp->cancellable != cancellable ? "cancellable flag" : "schema");
      else if (hp->doc == NULL && doc != NULL)
        hp->doc = g_strdup (doc);
      return;
    }

  hp = g_new0 (HookPoint, 1);
  hp->name = g_strdup (name);
  hp->schema = g_strdup (schema);
  hp->doc = g_strdup (doc);
  hp->cancellable = cancellable;
  hp->registry = self;
  for (int i = 0; i < N_PHASES; i++)
    hp->listeners[i] = g_ptr_array_new ();

  g_hash_table_insert (self->points, hp->name, hp);
  g_signal_emit (self, signals[SIGNAL_DECLARED], 0, name);
}

/**
 * shell_hook_registry_is_declared:
 * @self: the registry
 * @name: hook name
 */
gboolean
shell_hook_registry_is_declared (ShellHookRegistry *self,
                                 const char        *name)
{
  g_return_val_if_fail (SHELL_IS_HOOK_REGISTRY (self), FALSE);

  return lookup_point (self, name) != NULL;
}

/**
 * shell_hook_registry_is_cancellable:
 * @self: the registry
 * @name: hook name
 */
gboolean
shell_hook_registry_is_cancellable (ShellHookRegistry *self,
                                    const char        *name)
{
  HookPoint *hp;

  g_return_val_if_fail (SHELL_IS_HOOK_REGISTRY (self), FALSE);

  hp = lookup_point (self, name);
  return hp != NULL && hp->cancellable;
}

static int
compare_strings (gconstpointer a,
                 gconstpointer b)
{
  return strcmp (*(const char * const *) a, *(const char * const *) b);
}

/**
 * shell_hook_registry_list:
 * @self: the registry
 *
 * Returns: (transfer full) (array zero-terminated=1) (element-type utf8):
 *   the sorted names of all declared hook points
 */
GStrv
shell_hook_registry_list (ShellHookRegistry *self)
{
  GPtrArray *names;
  GHashTableIter iter;
  gpointer key;

  g_return_val_if_fail (SHELL_IS_HOOK_REGISTRY (self), NULL);

  names = g_ptr_array_new ();
  g_hash_table_iter_init (&iter, self->points);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_ptr_array_add (names, g_strdup (key));
  g_ptr_array_sort (names, compare_strings);
  g_ptr_array_add (names, NULL);

  return (GStrv) g_ptr_array_free (names, FALSE);
}

/**
 * shell_hook_registry_get_doc:
 * @self: the registry
 * @name: hook name
 *
 * Returns: (transfer none) (nullable): the documentation string
 */
const char *
shell_hook_registry_get_doc (ShellHookRegistry *self,
                             const char        *name)
{
  HookPoint *hp;

  g_return_val_if_fail (SHELL_IS_HOOK_REGISTRY (self), NULL);

  hp = lookup_point (self, name);
  return hp ? hp->doc : NULL;
}

/**
 * shell_hook_registry_get_schema:
 * @self: the registry
 * @name: hook name
 *
 * Returns: (transfer none) (nullable): the payload schema description
 */
const char *
shell_hook_registry_get_schema (ShellHookRegistry *self,
                                const char        *name)
{
  HookPoint *hp;

  g_return_val_if_fail (SHELL_IS_HOOK_REGISTRY (self), NULL);

  hp = lookup_point (self, name);
  return hp ? hp->schema : NULL;
}

/* ---- connect / disconnect --------------------------------------------- */

/* ---- replay ------------------------------------------------------------ */

static gboolean
replay_idle_cb (gpointer data)
{
  Listener *l = listener_ref (data);
  HookPoint *hp = l->point;
  GPtrArray *payloads = NULL;

  l->replay_source = 0;
  l->replay_pending = FALSE;

  if (!l->removed && hp->replay != NULL)
    {
      g_object_ref (hp->registry);
      payloads = hp->replay (hp->registry, hp->name, hp->replay_data);

      for (guint i = 0; payloads != NULL && i < payloads->len && !l->removed; i++)
        {
          GVariant *payload = g_ptr_array_index (payloads, i);

          if (payload == NULL || !g_variant_is_of_type (payload, G_VARIANT_TYPE ("a{sv}")))
            {
              g_warning ("Replay of hook '%s' returned a payload that is not a{sv}", hp->name);
              continue;
            }
          if (g_variant_is_floating (payload))
            g_variant_ref_sink (payload);   /* the array owns it */

          l->fn (hp->name, SHELL_HOOK_PHASE_AFTER, payload, NULL, l->user_data);
        }
      g_clear_pointer (&payloads, g_ptr_array_unref);
      g_object_unref (hp->registry);
    }

  listener_unref (l);
  return G_SOURCE_REMOVE;
}

/* shell_hook_registry_set_replay (): see shell-hooks.h */
void
shell_hook_registry_set_replay (ShellHookRegistry   *self,
                                const char          *name,
                                ShellHookReplayFunc  func,
                                gpointer             user_data)
{
  HookPoint *hp;

  g_return_if_fail (SHELL_IS_HOOK_REGISTRY (self));

  hp = lookup_point (self, name);
  if (hp == NULL)
    {
      g_warning ("Cannot set the replay of undeclared hook '%s'", name ? name : "(null)");
      return;
    }

  hp->replay = func;
  hp->replay_data = func != NULL ? user_data : NULL;
}

/**
 * shell_hook_registry_connect: (skip)
 * @self: the registry
 * @name: hook name
 * @phase: phase to listen to
 * @listener: callback
 * @user_data: data for @listener
 * @destroy: (nullable): called exactly once when the listener is released
 *
 * Returns: a handler id, or 0 on failure
 */
guint
shell_hook_registry_connect (ShellHookRegistry *self,
                             const char        *name,
                             ShellHookPhase     phase,
                             ShellHookListener  listener,
                             gpointer           user_data,
                             GDestroyNotify     destroy)
{
  HookPoint *hp;
  Listener *l;

  g_return_val_if_fail (SHELL_IS_HOOK_REGISTRY (self), 0);
  g_return_val_if_fail (listener != NULL, 0);
  g_return_val_if_fail (phase == SHELL_HOOK_PHASE_BEFORE ||
                        phase == SHELL_HOOK_PHASE_AFTER, 0);

  if (!hook_name_is_valid (name))
    {
      g_warning ("Cannot connect to hook with invalid name '%s'",
                 name ? name : "(null)");
      return 0;
    }

  hp = lookup_point (self, name);
  if (hp == NULL)
    {
      g_warning ("Cannot connect to undeclared hook '%s'", name);
      return 0;
    }

  if (phase == SHELL_HOOK_PHASE_BEFORE && !hp->cancellable)
    {
      g_warning ("Hook '%s' is not cancellable; it has no 'before' phase", name);
      return 0;
    }

  l = g_new0 (Listener, 1);
  l->id = self->next_id++;
  if (self->next_id == 0)
    self->next_id = 1;
  l->fn = listener;
  l->user_data = user_data;
  l->destroy = destroy;
  l->point = hp;
  l->phase = phase;
  l->ref_count = 1; /* owned by the array */

  g_ptr_array_add (hp->listeners[phase], l);
  g_hash_table_insert (self->by_id, GUINT_TO_POINTER (l->id), l);

  if (phase == SHELL_HOOK_PHASE_AFTER && hp->replay != NULL)
    {
      /* The replay runs after this returns: the listener is told about what
       * exists, not from inside the call that connected it. The source is
       * the listener's own (removed when it is freed). */
      l->replay_pending = TRUE;
      l->replay_source = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, replay_idle_cb, l, NULL);
    }

  return l->id;
}

/**
 * shell_hook_registry_disconnect:
 * @self: the registry
 * @id: a handler id returned by shell_hook_registry_connect()
 *
 * Safe to call from inside a listener, including for the running one.
 * Unknown ids (including 0) are ignored.
 */
void
shell_hook_registry_disconnect (ShellHookRegistry *self,
                                guint              id)
{
  Listener *l;
  GPtrArray *a;

  g_return_if_fail (SHELL_IS_HOOK_REGISTRY (self));

  if (id == 0)
    return;

  l = g_hash_table_lookup (self->by_id, GUINT_TO_POINTER (id));
  if (l == NULL)
    return;

  g_hash_table_remove (self->by_id, GUINT_TO_POINTER (id));
  l->removed = TRUE;

  a = l->point->listeners[l->phase];
  g_ptr_array_remove (a, l); /* order preserving; array has no free func */

  /* Drops the array's reference; destroy runs now unless an emission still
   * holds the listener in its snapshot. */
  listener_unref (l);
}

/**
 * shell_hook_registry_count_listeners:
 * @self: the registry
 * @name: hook name
 * @phase: phase
 *
 * Returns: the number of connected listeners (0 for undeclared hooks)
 */
guint
shell_hook_registry_count_listeners (ShellHookRegistry *self,
                                     const char        *name,
                                     ShellHookPhase     phase)
{
  HookPoint *hp;

  g_return_val_if_fail (SHELL_IS_HOOK_REGISTRY (self), 0);
  g_return_val_if_fail (phase == SHELL_HOOK_PHASE_BEFORE ||
                        phase == SHELL_HOOK_PHASE_AFTER, 0);

  hp = lookup_point (self, name);
  return hp ? hp->listeners[phase]->len : 0;
}

/* ---- emission --------------------------------------------------------- */

/* Returns a sunk, owned reference to a valid a{sv}; NULL on programmer error. */
static GVariant *
normalize_payload (GVariant *payload)
{
  if (payload == NULL)
    return g_variant_ref_sink (g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0));

  g_return_val_if_fail (g_variant_is_of_type (payload, G_VARIANT_TYPE ("a{sv}")), NULL);

  return g_variant_ref_sink (payload);
}

static GPtrArray *
snapshot_listeners (HookPoint      *hp,
                    ShellHookPhase  phase)
{
  GPtrArray *src = hp->listeners[phase];
  GPtrArray *snap = g_ptr_array_new_full (src->len, (GDestroyNotify) listener_unref);

  for (guint i = 0; i < src->len; i++)
    g_ptr_array_add (snap, listener_ref (g_ptr_array_index (src, i)));

  return snap;
}

static void
merge_amendments (GVariantDict *result,
                  GVariant     *amended)
{
  GVariantIter iter;
  const char *key;
  GVariant *value;

  g_variant_iter_init (&iter, amended);
  while (g_variant_iter_loop (&iter, "{&sv}", &key, &value))
    g_variant_dict_insert_value (result, key, value);
}

/*
 * shell_hook_registry_emit_before:
 * @self: the registry
 * @name: hook name
 * @payload: (nullable): a{sv}, NULL for an empty dictionary
 * @amendments: (out) (optional) (transfer full) (nullable): merged a{sv} of all amendments, NULL if none
 *
 * Returns: %SHELL_HOOK_VERDICT_CANCEL if any listener cancelled
 */
ShellHookVerdict
shell_hook_registry_emit_before (ShellHookRegistry *self,
                                 const char        *name,
                                 GVariant          *payload,
                                 GVariant         **amendments)
{
  HookPoint *hp;
  GVariant *pl;
  GPtrArray *snap;
  GVariantDict *merged = NULL;
  ShellHookVerdict verdict = SHELL_HOOK_VERDICT_CONTINUE;

  if (amendments)
    *amendments = NULL;

  g_return_val_if_fail (SHELL_IS_HOOK_REGISTRY (self), SHELL_HOOK_VERDICT_CONTINUE);

  pl = normalize_payload (payload);
  if (pl == NULL)
    return SHELL_HOOK_VERDICT_CONTINUE;

  hp = lookup_point (self, name);
  if (hp == NULL)
    {
      g_warning ("Emitting undeclared hook '%s'", name ? name : "(null)");
      g_variant_unref (pl);
      return SHELL_HOOK_VERDICT_CONTINUE;
    }
  if (!hp->cancellable)
    {
      g_warning ("Hook '%s' is not cancellable; use emit_after()", name);
      g_variant_unref (pl);
      return SHELL_HOOK_VERDICT_CONTINUE;
    }

  g_object_ref (self);
  g_signal_emit (self, signals[SIGNAL_EMITTED], 0, hp->name,
                 SHELL_HOOK_PHASE_BEFORE, pl);

  snap = snapshot_listeners (hp, SHELL_HOOK_PHASE_BEFORE);
  hp->emission_depth++;

  for (guint i = 0; i < snap->len; i++)
    {
      Listener *l = g_ptr_array_index (snap, i);
      GVariantDict *amend;
      GVariant *amended;
      ShellHookVerdict v;

      if (l->removed)
        continue;

      amend = g_variant_dict_new (NULL);
      v = l->fn (hp->name, SHELL_HOOK_PHASE_BEFORE, pl, amend, l->user_data);
      amended = g_variant_ref_sink (g_variant_dict_end (amend));
      g_variant_dict_unref (amend);

      if (g_variant_n_children (amended) > 0)
        {
          if (merged == NULL)
            merged = g_variant_dict_new (NULL);
          merge_amendments (merged, amended);
        }
      g_variant_unref (amended);

      if (v == SHELL_HOOK_VERDICT_CANCEL)
        {
          verdict = SHELL_HOOK_VERDICT_CANCEL;
          break;
        }
    }

  hp->emission_depth--;
  g_ptr_array_unref (snap); /* releases snapshot refs; deferred destroys run here */

  if (merged != NULL)
    {
      GVariant *result = g_variant_ref_sink (g_variant_dict_end (merged));

      g_variant_dict_unref (merged);
      if (amendments)
        *amendments = result;
      else
        g_variant_unref (result);
    }

  g_variant_unref (pl);
  g_object_unref (self);

  return verdict;
}

/*
 * shell_hook_registry_emit_after:
 * @self: the registry
 * @name: hook name
 * @payload: (nullable): a{sv}
 */
void
shell_hook_registry_emit_after (ShellHookRegistry *self,
                                const char        *name,
                                GVariant          *payload)
{
  HookPoint *hp;
  GVariant *pl;
  GPtrArray *snap;

  g_return_if_fail (SHELL_IS_HOOK_REGISTRY (self));

  pl = normalize_payload (payload);
  if (pl == NULL)
    return;

  hp = lookup_point (self, name);
  if (hp == NULL)
    {
      g_warning ("Emitting undeclared hook '%s'", name ? name : "(null)");
      g_variant_unref (pl);
      return;
    }

  g_object_ref (self);
  g_signal_emit (self, signals[SIGNAL_EMITTED], 0, hp->name,
                 SHELL_HOOK_PHASE_AFTER, pl);

  snap = snapshot_listeners (hp, SHELL_HOOK_PHASE_AFTER);
  hp->emission_depth++;

  for (guint i = 0; i < snap->len; i++)
    {
      Listener *l = g_ptr_array_index (snap, i);

      /* A listener whose replay is pending gets the state it describes from
       * the replay */
      if (l->removed || l->replay_pending)
        continue;

      l->fn (hp->name, SHELL_HOOK_PHASE_AFTER, pl, NULL, l->user_data);
    }

  hp->emission_depth--;
  g_ptr_array_unref (snap);

  g_variant_unref (pl);
  g_object_unref (self);
}
