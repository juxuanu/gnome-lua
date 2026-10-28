/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Unit tests for shell_util_fix_markup () (src/misc/text-utils.c), the markup
 * parsing of the message list. Port of tests/unit/markup.js: for every text,
 * the result with markup allowed and the one with it escaped must both be
 * valid Pango markup and equal the expected string, if there is one.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <pango/pango.h>

#include "misc/text-utils.h"

typedef struct
{
  const char *name;
  const char *text;
  const char *converted; /* NULL: only has to be valid markup */
  const char *escaped;
} MarkupCase;

static const MarkupCase markup_cases[] = {
  { "does not do anything on no markup",
    "foo",
    "foo",
    "foo" },
  { "converts and escapes bold markup",
    "<b>foo</b>",
    NULL,
    "&lt;b&gt;foo&lt;/b&gt;" },
  { "converts and escapes italic markup",
    "something <i>foo</i>",
    NULL,
    "something &lt;i&gt;foo&lt;/i&gt;" },
  { "converts and escapes underlined markup",
    "<u>foo</u> something",
    NULL,
    "&lt;u&gt;foo&lt;/u&gt; something" },
  { "converts and escapes non-nested bold italic and underline markup",
    "<b>bold</b> <i>italic <u>and underlined</u></i>",
    NULL,
    "&lt;b&gt;bold&lt;/b&gt; &lt;i&gt;italic &lt;u&gt;and underlined&lt;/u&gt;&lt;/i&gt;" },
  { "converts and escapes ampersands",
    "this &amp; that",
    NULL,
    "this &amp;amp; that" },
  { "converts and escapes <",
    "this &lt; that",
    NULL,
    "this &amp;lt; that" },
  { "converts and escapes >",
    "this &lt; that &gt; the other",
    NULL,
    "this &amp;lt; that &amp;gt; the other" },
  { "converts and escapes HTML markup inside escaped tags",
    "this &lt;<i>that</i>&gt;",
    NULL,
    "this &amp;lt;&lt;i&gt;that&lt;/i&gt;&amp;gt;" },
  { "converts and escapes angle brackets within HTML markup",
    "<b>this</b> > <i>that</i>",
    NULL,
    "&lt;b&gt;this&lt;/b&gt; &gt; &lt;i&gt;that&lt;/i&gt;" },
  { "converts and escapes markup whilst still keeping an unrecognized entity",
    "<b>smile</b> &#9786;!",
    NULL,
    "&lt;b&gt;smile&lt;/b&gt; &amp;#9786;!" },
  { "converts and escapes markup and a stray ampersand",
    "<b>this</b> & <i>that</i>",
    "<b>this</b> &amp; <i>that</i>",
    "&lt;b&gt;this&lt;/b&gt; &amp; &lt;i&gt;that&lt;/i&gt;" },
  { "converts and escapes a stray <",
    "this < that",
    "this &lt; that",
    "this &lt; that" },
  { "converts and escapes markup with a stray <",
    "<b>this</b> < <i>that</i>",
    "<b>this</b> &lt; <i>that</i>",
    "&lt;b&gt;this&lt;/b&gt; &lt; &lt;i&gt;that&lt;/i&gt;" },
  { "converts and escapes stray less than and greater than characters that do not form tags",
    "this < that > the other",
    "this &lt; that > the other",
    "this &lt; that &gt; the other" },
  { "converts and escapes stray less than and greater than characters next to HTML markup tags",
    "this <<i>that</i>>",
    "this &lt;<i>that</i>>",
    "this &lt;&lt;i&gt;that&lt;/i&gt;&gt;" },
  { "converts and escapes angle brackets around unknown tags",
    "<unknown>tag</unknown>",
    "&lt;unknown>tag&lt;/unknown>",
    "&lt;unknown&gt;tag&lt;/unknown&gt;" },
  { "converts and escapes angle brackets around unknown tags where the first letter might otherwise be valid HTML markup",
    "<bunknown>tag</bunknown>",
    "&lt;bunknown>tag&lt;/bunknown>",
    "&lt;bunknown&gt;tag&lt;/bunknown&gt;" },
  { "converts good tags but escapes bad tags",
    "<i>known</i> and <unknown>tag</unknown>",
    "<i>known</i> and &lt;unknown>tag&lt;/unknown>",
    "&lt;i&gt;known&lt;/i&gt; and &lt;unknown&gt;tag&lt;/unknown&gt;" },
  { "completely escapes mismatched tags where the mismatch is at the beginning",
    "<b>in<i>com</i>plete",
    "&lt;b&gt;in&lt;i&gt;com&lt;/i&gt;plete",
    "&lt;b&gt;in&lt;i&gt;com&lt;/i&gt;plete" },
  { "completely escapes mismatched tags where the mismatch is at the end",
    "in<i>com</i>plete</b>",
    "in&lt;i&gt;com&lt;/i&gt;plete&lt;/b&gt;",
    "in&lt;i&gt;com&lt;/i&gt;plete&lt;/b&gt;" },
  { "escapes all tags where there are attributes",
    "<b>good</b> and <b style='bad'>bad</b>",
    "&lt;b&gt;good&lt;/b&gt; and &lt;b style=&apos;bad&apos;&gt;bad&lt;/b&gt;",
    "&lt;b&gt;good&lt;/b&gt; and &lt;b style=&apos;bad&apos;&gt;bad&lt;/b&gt;" },
  { "escapes all tags where syntax is invalid",
    "<b>unrecognized</b stuff>",
    "&lt;b&gt;unrecognized&lt;/b stuff&gt;",
    "&lt;b&gt;unrecognized&lt;/b stuff&gt;" },
  { "escapes completely mismatched tags",
    "<b>mismatched</i>",
    "&lt;b&gt;mismatched&lt;/i&gt;",
    "&lt;b&gt;mismatched&lt;/i&gt;" },
  { "escapes mismatched tags where the first character is mismatched",
    "<b>mismatched/unknown</bunknown>",
    "&lt;b&gt;mismatched/unknown&lt;/bunknown&gt;",
    "&lt;b&gt;mismatched/unknown&lt;/bunknown&gt;" },
};

/* @expected is NULL if the markup only has to be valid (the cases of the JS
 * spec that did not give an expected string) */
static void
assert_parses_and_matches (const char *actual,
                           const char *expected)
{
  g_autoptr (GError) error = NULL;

  g_assert_true (pango_parse_markup (actual, -1, 0, NULL, NULL, NULL, &error));
  g_assert_no_error (error);
  if (expected != NULL)
    g_assert_cmpstr (actual, ==, expected);
}

static void
test_fix_markup (gconstpointer data)
{
  const MarkupCase *test_case = data;
  g_autofree char *converted = shell_util_fix_markup (test_case->text, TRUE);
  g_autofree char *escaped = shell_util_fix_markup (test_case->text, FALSE);

  g_test_message ("%s", test_case->name);
  assert_parses_and_matches (converted, test_case->converted);
  assert_parses_and_matches (escaped, test_case->escaped);
}

int
main (int argc, char **argv)
{
  guint i;

  g_test_init (&argc, &argv, NULL);

  for (i = 0; i < G_N_ELEMENTS (markup_cases); i++)
    {
      g_autofree char *slug = g_ascii_strdown (markup_cases[i].name, -1);
      g_autofree char *path = NULL;

      g_strcanon (slug, "abcdefghijklmnopqrstuvwxyz0123456789", '-');
      path = g_strdup_printf ("/shell/util/fix-markup/%02u-%s", i, slug);
      g_test_add_data_func (path, &markup_cases[i], test_fix_markup);
    }

  return g_test_run ();
}
