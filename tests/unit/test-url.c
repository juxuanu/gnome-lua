/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Unit tests for shell_util_find_urls () (src/misc/text-utils.c), the URLification
 * of the message list. Port of tests/unit/url.js: the same inputs, the same
 * URLs at the same positions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib.h>

#include "misc/text-utils.h"

typedef struct
{
  const char *url;
  int         pos;
} ExpectedUrl;

typedef struct
{
  const char *input;
  guint       n_urls;
  ExpectedUrl urls[2];
} UrlCase;

static const UrlCase url_cases[] = {
  { "This is a test", 0, {  } },
  { "This is http://www.gnome.org a test", 1, { { "http://www.gnome.org", 8 } } },
  { "This is http://www.gnome.org", 1, { { "http://www.gnome.org", 8 } } },
  { "http://www.gnome.org a test", 1, { { "http://www.gnome.org", 0 } } },
  { "http://www.gnome.org", 1, { { "http://www.gnome.org", 0 } } },
  { "Go to http://www.gnome.org.", 1, { { "http://www.gnome.org", 6 } } },
  { "Go to http://www.gnome.org/.", 1, { { "http://www.gnome.org/", 6 } } },
  { "(Go to http://www.gnome.org!)", 1, { { "http://www.gnome.org", 7 } } },
  { "Use GNOME (http://www.gnome.org).", 1, { { "http://www.gnome.org", 11 } } },
  { "This is a http://www.gnome.org/path test.", 1, { { "http://www.gnome.org/path", 10 } } },
  { "This is a www.gnome.org scheme-less test.", 1, { { "www.gnome.org", 10 } } },
  { "This is a www.gnome.org/scheme-less test.", 1, { { "www.gnome.org/scheme-less", 10 } } },
  { "This is a gnome.org/scheme-less test without www.", 1, { { "gnome.org/scheme-less", 10 } } },
  { "This is a status.gnome.org/scheme-less test.", 1, { { "status.gnome.org/scheme-less", 10 } } },
  { "This is a http://www.gnome.org:99/port test.", 1, { { "http://www.gnome.org:99/port", 10 } } },
  { "This is an ftp://www.gnome.org/ test.", 1, { { "ftp://www.gnome.org/", 11 } } },
  { "https://www.gnome.org/(some_url,_with_very_unusual_characters)", 1, { { "https://www.gnome.org/(some_url,_with_very_unusual_characters)", 0 } } },
  { "https://www.gnome.org/(some_url_with_unbalanced_parenthesis", 1, { { "https://www.gnome.org/", 0 } } },
  { "https://www.gnome.org/\xe2\x80\x8e"" plus trailing junk", 1, { { "https://www.gnome.org/", 0 } } },
  { "Visit http://www.gnome.org/ and http://developer.gnome.org", 2, { { "http://www.gnome.org/", 6 }, { "http://developer.gnome.org", 32 } } },
  { "This is not.a.domain test.", 0, {  } },
  { "This is not:a.url test.", 0, {  } },
  { "This is not:/a.url/ test.", 0, {  } },
  { "This is not:/a.url/ test.", 0, {  } },
  { "This is not@a.url/ test.", 0, {  } },
  { "This is surely@not.a/url test.", 0, {  } },
  { "This is not..aa/url test.", 0, {  } },
  { "This is ..not/a-url test.", 0, {  } },
  { "This is .absolutely.not/a-url test.", 0, {  } },
};

static void
test_find_urls (gconstpointer data)
{
  const UrlCase *test_case = data;
  GList *matches = shell_util_find_urls (test_case->input);
  GList *l;
  guint i = 0;

  g_assert_cmpuint (g_list_length (matches), ==, test_case->n_urls);

  for (l = matches; l != NULL; l = l->next, i++)
    {
      const ShellUrlMatch *match = l->data;

      g_assert_cmpstr (match->url, ==, test_case->urls[i].url);
      g_assert_cmpint (match->pos, ==, test_case->urls[i].pos);
    }

  g_list_free_full (matches, (GDestroyNotify) shell_url_match_free);
}

int
main (int argc, char **argv)
{
  guint i;

  g_test_init (&argc, &argv, NULL);

  for (i = 0; i < G_N_ELEMENTS (url_cases); i++)
    {
      g_autofree char *path = g_strdup_printf ("/shell/util/find-urls/%s/%02u",
                                               url_cases[i].n_urls > 0 ? "finds" : "does-not-find",
                                               i);

      g_test_add_data_func (path, &url_cases[i], test_find_urls);
    }

  return g_test_run ();
}
