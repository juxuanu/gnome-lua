/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ShellHookRegistry: named lifecycle hook points.
 *
 * The shell declares hook points ("window.focused", "overview.showing", ...)
 * with a payload schema and whether the "before" phase may cancel. C code (and,
 * until it is ported, JS code via GI) emits them; extensions listen through
 * shell.hooks. Listener lists are snapshotted on emit, so connecting or
 * disconnecting from inside a listener is safe. Emission is synchronous.
 *
 * Naming: "<subject>.<event>", lower-case, dots only. A cancellable hook gets
 * both phases; the "before" phase runs listeners in registration order and
 * stops at the first CANCEL. Listeners may amend the payload through the
 * GVariantDict; the emitter merges amendments it understands.
 *
 * This object is introspectable (ShellLua-1.gir) so JS can emit hooks.
 */
#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

typedef enum {
  SHELL_HOOK_VERDICT_CONTINUE,
  SHELL_HOOK_VERDICT_CANCEL,
} ShellHookVerdict;

#define SHELL_TYPE_HOOK_VERDICT (shell_hook_verdict_get_type ())
GType shell_hook_verdict_get_type (void) G_GNUC_CONST;

typedef enum {
  SHELL_HOOK_PHASE_BEFORE,
  SHELL_HOOK_PHASE_AFTER,
} ShellHookPhase;

#define SHELL_TYPE_HOOK_PHASE (shell_hook_phase_get_type ())
GType shell_hook_phase_get_type (void) G_GNUC_CONST;

/**
 * ShellHookListener:
 * @name: hook name
 * @phase: which phase is running
 * @payload: (transfer none): an a{sv} dictionary
 * @amend: (transfer none) (nullable): dictionary to put amendments in; NULL in the AFTER phase
 * @user_data: (closure)
 *
 * Returns: the verdict; ignored in the AFTER phase
 */
typedef ShellHookVerdict (*ShellHookListener) (const char     *name,
                                               ShellHookPhase  phase,
                                               GVariant       *payload,
                                               GVariantDict   *amend,
                                               gpointer        user_data);

#define SHELL_TYPE_HOOK_REGISTRY (shell_hook_registry_get_type ())
G_DECLARE_FINAL_TYPE (ShellHookRegistry, shell_hook_registry, SHELL, HOOK_REGISTRY, GObject)

ShellHookRegistry *shell_hook_registry_get_default (void);

/* Declaring is idempotent; redeclaring with a different schema/cancellable
 * is a programmer error (g_critical). */
void               shell_hook_registry_declare     (ShellHookRegistry *self,
                                                    const char        *name,
                                                    const char        *payload_schema, /* human doc of keys, e.g. "title:s window:o" */
                                                    gboolean           cancellable,
                                                    const char        *doc);
gboolean           shell_hook_registry_is_declared (ShellHookRegistry *self,
                                                    const char        *name);
gboolean           shell_hook_registry_is_cancellable (ShellHookRegistry *self,
                                                       const char        *name);
/* (transfer full) sorted */
GStrv              shell_hook_registry_list        (ShellHookRegistry *self);
/* (transfer none) (nullable) */
const char        *shell_hook_registry_get_doc     (ShellHookRegistry *self,
                                                    const char        *name);
const char        *shell_hook_registry_get_schema  (ShellHookRegistry *self,
                                                    const char        *name);

/* Returns 0 (with g_warning) if @name is not declared, or if @phase is BEFORE
 * on a non-cancellable hook. (skip) */
guint              shell_hook_registry_connect     (ShellHookRegistry *self,
                                                    const char        *name,
                                                    ShellHookPhase     phase,
                                                    ShellHookListener  listener,
                                                    gpointer           user_data,
                                                    GDestroyNotify     destroy);
void               shell_hook_registry_disconnect  (ShellHookRegistry *self,
                                                    guint              id);
guint              shell_hook_registry_count_listeners (ShellHookRegistry *self,
                                                        const char        *name,
                                                        ShellHookPhase     phase);

/**
 * ShellHookReplayFunc:
 * @registry: the registry
 * @name: the hook name
 * @user_data: (closure): the data given to shell_hook_registry_set_replay()
 *
 * Called by the registry, from an idle, for a listener that connected to the
 * "after" phase of a hook that has a replay. Returns the events that describe
 * what exists now, in the order they are to be delivered. The function must
 * answer from the state as it is when called, including everything that was
 * emitted already, because the new listener does not receive live emissions
 * that happen between its connection and the replay.
 *
 * Returns: (transfer full) (element-type GVariant) (nullable): an array of
 *   a{sv} payloads, owned by the array (no floating references); %NULL for
 *   none
 */
typedef GPtrArray * (*ShellHookReplayFunc) (ShellHookRegistry *registry,
                                            const char        *name,
                                            gpointer           user_data);

/**
 * shell_hook_registry_set_replay: (skip)
 * @self: the registry
 * @name: a declared hook
 * @func: (nullable): the replay function, %NULL removes it
 * @user_data: data for @func; must stay valid as long as the replay is set
 *
 * Makes @name a hook with a replay: whenever a listener connects to its
 * "after" phase, @func is called from an idle (never from inside
 * shell_hook_registry_connect()) and each payload it returns is delivered to
 * that one listener, as an "after" emission that no other listener sees (the
 * "emitted" signal is not emitted). Until the replay has run, the listener
 * is skipped by the live emissions of the hook, since the replay covers them.
 * A listener that is disconnected before the idle runs gets nothing. Listeners
 * that were connected before the replay was set get none.
 */
void               shell_hook_registry_set_replay  (ShellHookRegistry *self,
                                                    const char        *name,
                                                    ShellHookReplayFunc func,
                                                    gpointer           user_data);

/**
 * shell_hook_registry_emit_before:
 * @payload: (nullable): a{sv}, NULL for an empty dictionary
 * @amendments: (out) (optional) (transfer full) (nullable): merged a{sv} of all amendments, NULL if none
 *
 * Returns: %SHELL_HOOK_VERDICT_CANCEL if any listener cancelled
 */
ShellHookVerdict   shell_hook_registry_emit_before (ShellHookRegistry *self,
                                                    const char        *name,
                                                    GVariant          *payload,
                                                    GVariant         **amendments);

/**
 * shell_hook_registry_emit_after:
 * @payload: (nullable): a{sv}
 */
void               shell_hook_registry_emit_after  (ShellHookRegistry *self,
                                                    const char        *name,
                                                    GVariant          *payload);

/* Signals:
 *   "declared" (const char *name)
 *   "emitted"  (const char *name, ShellHookPhase phase, GVariant *payload) -- for Looking Glass tracing
 */

G_END_DECLS
