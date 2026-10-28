/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * extension-order.c: The ordering rule for the items that extensions add to
 * the shell. See extension-order.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <string.h>

#include <gio/gio.h>

#include "shell-global.h"
#include "ui/extension-order.h"

/**
 * shell_extension_order_get_rank:
 * @uuid: the uuid of an extension
 *
 * Gets the position of @uuid in the `enabled-extensions` setting of
 * org.gnome.shell.
 *
 * Returns: the 0-based position, or %G_MAXINT if the extension is not listed
 */
int
shell_extension_order_get_rank (const char *uuid)
{
  GSettings *settings;
  g_auto (GStrv) enabled = NULL;
  int i;

  if (uuid == NULL)
    return G_MAXINT;

  settings = shell_global_get_settings (shell_global_get ());
  if (settings == NULL)
    return G_MAXINT;

  enabled = g_settings_get_strv (settings, "enabled-extensions");
  for (i = 0; enabled[i] != NULL; i++)
    {
      if (strcmp (enabled[i], uuid) == 0)
        return i;
    }

  return G_MAXINT;
}

/**
 * shell_extension_order_compare:
 * @uuid_a: the uuid of an extension
 * @uuid_b: the uuid of another extension
 *
 * Compares two extensions by their position in `enabled-extensions`, the
 * ones that are not listed (which should not own items for long) after the
 * others and by uuid among themselves.
 *
 * Returns: negative if @uuid_a goes first, positive if @uuid_b does, 0 if
 *   they are the same extension
 */
int
shell_extension_order_compare (const char *uuid_a,
                               const char *uuid_b)
{
  int rank_a, rank_b;

  if (g_strcmp0 (uuid_a, uuid_b) == 0)
    return 0;

  rank_a = shell_extension_order_get_rank (uuid_a);
  rank_b = shell_extension_order_get_rank (uuid_b);

  if (rank_a != rank_b)
    return rank_a < rank_b ? -1 : 1;

  return g_strcmp0 (uuid_a, uuid_b);
}

/**
 * shell_extension_order_insert_position:
 * @items: the items, grouped by owner and ordered by the owners' rank
 * @owner_of: gets the owner of an item
 * @in_slot: tells whether an item is in the slot that is being added to
 * @slot_data: data for @in_slot
 * @owner: the owner of the new item
 * @index: the 1-based position among the owner's items in the slot, or 0 or
 *   less to go after the last one; values past the end clamp
 *
 * Finds where in @items an item of @owner must be inserted to be at @index
 * among the items of @owner in the slot.
 *
 * Returns: the index to insert at in @items
 */
guint
shell_extension_order_insert_position (GPtrArray                    *items,
                                       ShellExtensionOrderOwnerFunc   owner_of,
                                       ShellExtensionOrderSlotFunc    in_slot,
                                       gpointer                       slot_data,
                                       const char                    *owner,
                                       int                            index)
{
  guint i, end_of_owner = items->len;
  int seen = 0;
  gboolean found_owner = FALSE;

  for (i = 0; i < items->len; i++)
    {
      gpointer item = g_ptr_array_index (items, i);
      const char *item_owner = owner_of (item);

      if (strcmp (item_owner, owner) == 0)
        {
          found_owner = TRUE;
          end_of_owner = i + 1;

          if (in_slot (item, slot_data))
            {
              seen++;
              if (index > 0 && seen == index)
                return i;
            }
        }
      else if (!found_owner && shell_extension_order_compare (item_owner, owner) > 0)
        {
          /* The first item of an extension that goes after @owner, and
           * @owner has no items yet */
          end_of_owner = i;
          break;
        }
      else if (found_owner)
        {
          break;
        }
    }

  return end_of_owner;
}

typedef struct
{
  ShellExtensionOrderOwnerFunc owner_of;
} SortData;

static int
compare_items (gconstpointer a,
               gconstpointer b,
               gpointer      user_data)
{
  SortData *data = user_data;
  gpointer item_a = *(gpointer const *) a;
  gpointer item_b = *(gpointer const *) b;

  return shell_extension_order_compare (data->owner_of (item_a), data->owner_of (item_b));
}

/**
 * shell_extension_order_sort:
 * @items: the items
 * @owner_of: gets the owner of an item
 *
 * Sorts @items by the rank of their owners. The sort is stable, so the items
 * of one owner keep their order.
 */
void
shell_extension_order_sort (GPtrArray                    *items,
                            ShellExtensionOrderOwnerFunc  owner_of)
{
  SortData data = { owner_of };

  g_ptr_array_sort_with_data (items, compare_items, &data);
}
