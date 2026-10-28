/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * extension-order.h: The ordering rule for the items that extensions add to
 * the shell (docs/api/README.md, "Placement and ordering"): inside a slot,
 * extension items are ordered by the position of their extension in the
 * `enabled-extensions` setting of org.gnome.shell and then by the position
 * the extension asked for among its own items of that slot. The order is a
 * pure function of the setting and of those requests, never of the time the
 * extensions were enabled.
 *
 * A slot is kept in a #GPtrArray of items; the helpers need to know the owner
 * (the uuid of the extension) of an item and, for the insertion, which items
 * belong to the same slot (a slot array may be shared by several slots, as
 * the panel does for its boxes and edges).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef const char *(* ShellExtensionOrderOwnerFunc) (gpointer item);
typedef gboolean    (* ShellExtensionOrderSlotFunc)  (gpointer item,
                                                      gpointer slot_data);

int   shell_extension_order_get_rank  (const char *uuid);
int   shell_extension_order_compare   (const char *uuid_a,
                                       const char *uuid_b);

guint shell_extension_order_insert_position (GPtrArray                    *items,
                                            ShellExtensionOrderOwnerFunc   owner_of,
                                            ShellExtensionOrderSlotFunc    in_slot,
                                            gpointer                       slot_data,
                                            const char                    *owner,
                                            int                            index);

void  shell_extension_order_sort      (GPtrArray                    *items,
                                       ShellExtensionOrderOwnerFunc  owner_of);

G_END_DECLS
