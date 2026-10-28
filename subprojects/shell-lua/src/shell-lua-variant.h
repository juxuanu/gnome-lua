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

G_END_DECLS
