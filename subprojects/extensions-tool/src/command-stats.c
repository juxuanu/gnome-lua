/* command-stats.c
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* What the running extensions cost: the time of their callbacks since they
 * were enabled, the longest one and where it is, the callbacks longer than a
 * frame, and their Lua memory (the "callback*", "slowCallbacks" and "memory"
 * keys of the extension info). The most expensive first. */

#include "config.h"

#include <string.h>

#include <gio/gio.h>
#include <glib/gi18n.h>

#include "commands.h"
#include "common.h"

typedef struct {
  char   *uuid;
  double  callbacks;
  double  time;
  double  longest;
  char   *where;
  double  slow;
  double  memory;
} Row;

static void
row_free (Row *row)
{
  g_free (row->uuid);
  g_free (row->where);
  g_free (row);
}

static double
row_key (const Row  *row,
         const char *sort)
{
  if (g_str_equal (sort, "longest"))
    return row->longest;
  if (g_str_equal (sort, "slow"))
    return row->slow;
  if (g_str_equal (sort, "memory"))
    return row->memory;
  if (g_str_equal (sort, "callbacks"))
    return row->callbacks;
  return row->time;
}

static int
compare_rows (gconstpointer a,
              gconstpointer b,
              gpointer      sort)
{
  const Row *ra = *(Row **) a;
  const Row *rb = *(Row **) b;
  double ka = row_key (ra, sort);
  double kb = row_key (rb, sort);

  if (ka != kb)
    return ka < kb ? 1 : -1;
  return g_strcmp0 (ra->uuid, rb->uuid);
}

static gboolean
show_stats (const char *sort)
{
  g_autoptr (GDBusProxy) proxy = NULL;
  g_autoptr (GVariant) response = NULL;
  g_autoptr (GVariant) extensions = NULL;
  g_autoptr (GPtrArray) rows = g_ptr_array_new_with_free_func ((GDestroyNotify) row_free);
  g_autoptr (GError) error = NULL;
  GVariantIter iter;
  GVariant *value;
  char *uuid;
  int width = strlen (_("EXTENSION"));
  guint i;

  proxy = get_shell_proxy (&error);
  if (proxy == NULL)
    return FALSE;

  response = g_dbus_proxy_call_sync (proxy,
                                     "ListExtensions",
                                     NULL,
                                     0,
                                     -1,
                                     NULL,
                                     &error);
  if (response == NULL)
    {
      g_printerr (_("Failed to connect to GNOME Shell\n"));
      return FALSE;
    }

  extensions = g_variant_get_child_value (response, 0);

  /* Only the extensions that run have figures */
  g_variant_iter_init (&iter, extensions);
  while (g_variant_iter_loop (&iter, "{s@a{sv}}", &uuid, &value))
    {
      g_autoptr (GVariantDict) info = g_variant_dict_new (value);
      Row *row;
      const char *where = "";

      if (!g_variant_dict_contains (info, "callbacks"))
        continue;

      row = g_new0 (Row, 1);
      row->uuid = g_strdup (uuid);
      g_variant_dict_lookup (info, "callbacks", "d", &row->callbacks);
      g_variant_dict_lookup (info, "callbackTime", "d", &row->time);
      g_variant_dict_lookup (info, "callbackLongest", "d", &row->longest);
      g_variant_dict_lookup (info, "callbackLongestWhere", "&s", &where);
      g_variant_dict_lookup (info, "slowCallbacks", "d", &row->slow);
      g_variant_dict_lookup (info, "memory", "d", &row->memory);
      row->where = g_strdup (where);
      g_ptr_array_add (rows, row);

      width = MAX (width, (int) strlen (uuid));
    }

  if (rows->len == 0)
    {
      g_print ("%s\n", _("No extension is running"));
      return TRUE;
    }

  g_ptr_array_sort_with_data (rows, compare_rows, (gpointer) sort);

  /* TRANSLATORS: the column headers of “gnome-extensions stats”: the
   * extension, the number of its callbacks, their time in total, the
   * longest one, the callbacks longer than a frame, its Lua memory, and
   * where the longest callback is defined */
  g_print ("%-*s %10s %11s %10s %6s %9s  %s\n", width, _("EXTENSION"),
           _("CALLBACKS"), _("TOTAL"), _("LONGEST"), _("SLOW"), _("MEMORY"), _("WHERE"));

  for (i = 0; i < rows->len; i++)
    {
      Row *row = g_ptr_array_index (rows, i);
      g_autofree char *memory = g_format_size ((guint64) row->memory);
      g_autofree char *total = g_strdup_printf ("%.1f ms", row->time);
      g_autofree char *longest = g_strdup_printf ("%.1f ms", row->longest);

      g_print ("%-*s %10.0f %11s %10s %6.0f %9s  %s\n", width, row->uuid,
               row->callbacks, total, longest, row->slow, memory, row->where);
    }

  return TRUE;
}

int
handle_stats (int argc, char *argv[], gboolean do_help)
{
  g_autoptr (GOptionContext) context = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *sort = NULL;
  GOptionEntry entries[] = {
    { .long_name = "sort", .short_name = 's',
      .arg = G_OPTION_ARG_STRING, .arg_data = &sort,
      .description = _("Sort by: time (default), longest, slow, callbacks or memory"),
      .arg_description = _("KEY") },
    { NULL }
  };

  g_set_prgname ("gnome-extensions stats");

  context = g_option_context_new (NULL);
  g_option_context_set_help_enabled (context, FALSE);
  g_option_context_set_summary (context,
                                _("Show what the running extensions cost: the time "
                                  "of their callbacks since they were enabled, the "
                                  "callbacks longer than a frame, and their memory"));
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_add_group (context, get_option_group());

  if (do_help)
    {
      show_help (context, NULL);
      return 0;
    }

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      show_help (context, error->message);
      return 1;
    }

  if (argc > 1)
    {
      show_help (context, _("Unknown arguments"));
      return 1;
    }

  if (sort == NULL)
    sort = g_strdup ("time");

  if (!g_str_equal (sort, "time") && !g_str_equal (sort, "longest") &&
      !g_str_equal (sort, "slow") && !g_str_equal (sort, "callbacks") &&
      !g_str_equal (sort, "memory"))
    {
      show_help (context, _("Unknown sort key"));
      return 1;
    }

  return show_stats (sort) ? 0 : 2;
}
