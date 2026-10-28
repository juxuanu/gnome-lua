/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Unit tests for ShellHighlighter (src/ui/search.c), the highlighter of the
 * descriptions of the search results. Port of tests/unit/highlighter.js: the
 * same 22 inputs and outputs, and the output must be valid Pango markup.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <pango/pango.h>

#include "ui/search.h"

typedef struct
{
  const char         *input;
  const char * const *terms;
  const char         *output;
} HighlightCase;

static const HighlightCase highlight_cases[] = {
  { "abc cba",
    NULL,
    "abc cba" },
  { "abc cba",
    (const char * const []) { NULL },
    "abc cba" },
  { "abc cba",
    (const char * const []) { "", NULL },
    "abc cba" },
  { "abc cba",
    (const char * const []) { "a", NULL },
    "<b>a</b>bc cb<b>a</b>" },
  { "abc cba",
    (const char * const []) { "a", "a", NULL },
    "<b>a</b>bc cb<b>a</b>" },
  { "CaSe InSenSiTiVe",
    (const char * const []) { "cas", "sens", NULL },
    "<b>CaS</b>e In<b>SenS</b>iTiVe" },
  { "This contains the < character",
    NULL,
    "This contains the &lt; character" },
  { "Don't",
    (const char * const []) { "t", NULL },
    "Don&apos;<b>t</b>" },
  { "Don't",
    (const char * const []) { "n't", NULL },
    "Do<b>n&apos;t</b>" },
  { "Don't",
    (const char * const []) { "o", "t", NULL },
    "D<b>o</b>n&apos;<b>t</b>" },
  { "salt&pepper",
    (const char * const []) { "salt", NULL },
    "<b>salt</b>&amp;pepper" },
  { "salt&pepper",
    (const char * const []) { "salt", "alt", NULL },
    "<b>salt</b>&amp;pepper" },
  { "salt&pepper",
    (const char * const []) { "pepper", NULL },
    "salt&amp;<b>pepper</b>" },
  { "salt&pepper",
    (const char * const []) { "salt", "pepper", NULL },
    "<b>salt</b>&amp;<b>pepper</b>" },
  { "salt&pepper",
    (const char * const []) { "t", "p", NULL },
    "sal<b>t</b>&amp;<b>p</b>e<b>p</b><b>p</b>er" },
  { "salt&pepper",
    (const char * const []) { "t", "&", "p", NULL },
    "sal<b>t</b><b>&amp;</b><b>p</b>e<b>p</b><b>p</b>er" },
  { "salt&pepper",
    (const char * const []) { "e", NULL },
    "salt&amp;p<b>e</b>pp<b>e</b>r" },
  { "salt&pepper",
    (const char * const []) { "&a", "&am", "&amp", "&amp;", NULL },
    "salt&amp;pepper" },
  { "&&&&&",
    (const char * const []) { "a", NULL },
    "&amp;&amp;&amp;&amp;&amp;" },
  { "&;&;&;&;&;",
    (const char * const []) { "a", NULL },
    "&amp;;&amp;;&amp;;&amp;;&amp;;" },
  { "&;&;&;&;&;",
    (const char * const []) { ";", NULL },
    "&amp;<b>;</b>&amp;<b>;</b>&amp;<b>;</b>&amp;<b>;</b>&amp;<b>;</b>" },
  { "&amp;",
    (const char * const []) { "a", NULL },
    "&amp;<b>a</b>mp;" },
};

static void
test_highlight (gconstpointer data)
{
  const HighlightCase *test_case = data;
  g_autoptr (ShellHighlighter) highlighter = shell_highlighter_new (test_case->terms);
  g_autoptr (GError) error = NULL;
  g_autofree char *output = shell_highlighter_highlight (highlighter, test_case->input);

  g_assert_cmpstr (output, ==, test_case->output);
  g_assert_true (pango_parse_markup (output, -1, 0, NULL, NULL, NULL, &error));
  g_assert_no_error (error);
}

int
main (int argc, char **argv)
{
  guint i;

  g_test_init (&argc, &argv, NULL);

  for (i = 0; i < G_N_ELEMENTS (highlight_cases); i++)
    {
      g_autofree char *path = g_strdup_printf ("/shell/highlighter/%02u", i);

      g_test_add_data_func (path, &highlight_cases[i], test_highlight);
    }

  return g_test_run ();
}
