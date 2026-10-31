/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * GVariant <-> Lua. Used by hooks payloads, D-Bus (later) and settings.
 *
 * push:   b -> boolean; y n q i u x t h -> integer; d -> number;
 *         s o g -> string; v -> unwrapped; m* -> nil or inner;
 *         a{s*} and a{o*} -> table keyed by string; other a* and tuples ->
 *         sequence (1-based); ay -> string (bytes).
 * to:     with a type: converts or raises a Lua error naming the mismatch;
 *         without a type: boolean->b, integer->x, number->d, string->s,
 *         sequence->av, other table->a{sv}, nil->maybe (raises: ambiguous).
 *
 * Typed values: shell_lua_push_variant_box() pushes an opaque Lua value
 * wrapping a GVariant of an exact type (shell.dbus.variant). A `v` target
 * position accepts it in place of a plain Lua value; any other position,
 * and the untyped conversion, rejects it.
 */
#pragma once

#include <glib.h>
#include <lua.h>

G_BEGIN_DECLS

void      shell_lua_push_variant (lua_State          *L,
                                  GVariant           *variant);

/* Returns a floating-sunk, ref'd GVariant or NULL after raising a Lua error. */
GVariant *shell_lua_to_variant   (lua_State          *L,
                                  int                 idx,
                                  const GVariantType *type);   /* nullable */

typedef enum {
  SHELL_LUA_VARIANT_NONE        = 0,
  /* In the conversion without a type, a string that is not valid UTF-8 (or
   * has an embedded NUL) becomes `ay` instead of failing; it comes back as
   * a Lua string of the same bytes (worker messages, docs/api/worker.md). */
  SHELL_LUA_VARIANT_BYTES_AS_AY = 1 << 0,
  /* In the conversion without a type, a table with both integer and string
   * keys fails instead of becoming an `a{sv}` without its integer keys
   * (a message must arrive whole or not at all). */
  SHELL_LUA_VARIANT_STRICT_KEYS = 1 << 1,
} ShellLuaVariantFlags;

/* Same with @flags, and a cap on the number of values (every scalar and
 * every container counts one; 0: no cap): beyond it the conversion fails
 * with `too many values (limit N)` before it builds the rest. */
GVariant *shell_lua_to_variant_full (lua_State          *L,
                                     int                 idx,
                                     const GVariantType *type,
                                     guint               flags,
                                     gsize               max_values);

/* Pushes a typed-value box holding a reference to @variant (sunk if floating).
 * Boxes are immutable and compare by identity. */
void      shell_lua_push_variant_box (lua_State          *L,
                                      GVariant           *variant);

/* The GVariant inside the box at @idx (transfer none), or NULL when the value
 * is not a box. Never raises. */
GVariant *shell_lua_variant_box_peek (lua_State          *L,
                                      int                 idx);

G_END_DECLS
