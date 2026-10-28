/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * text-utils.c: Text helpers of the message list: finding URLs in a text and
 * fixing up embedded markup. Ports of `findUrls ()` and `fixMarkup ()` of
 * js/misc/util.js.
 *
 * The positions of the matches are in characters, like the offsets of the
 * #ClutterText of the message list; the JS function reported UTF-16 code
 * units, which only differ for characters outside the Basic Multilingual
 * Plane. The match also carries the offset in bytes for C callers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <pango/pango.h>

#include "misc/text-utils.h"

/* http://daringfireball.net/2010/07/improved_regex_for_matching_urls */
#define BALANCED_PARENS "\\([^\\s()<>]+\\)"
#define LEADING_JUNK "[\\s`(\\[{'\\\"<\\x{00AB}\\x{201C}\\x{2018}]"
#define NOT_TRAILING_JUNK "[^\\s`!()\\[\\]{};:'\\\".,<>?\\x{00AB}\\x{00BB}\\x{200E}\\x{200F}" \
                          "\\x{201C}\\x{201D}\\x{2018}\\x{2019}\\x{202A}\\x{202C}]"

#define URL_REGEXP \
  "(^|" LEADING_JUNK ")" \
  "(" \
    "(?:" \
      "(?:http|https|ftp)://" /* scheme:// */ \
      "|" \
      "www[0-9]{0,3}[.]" /* www. */ \
      "|" \
      "([a-z0-9\\-]+[.])+[a-z]{2,4}/" /* foo.xx/ */ \
    ")" \
    "(?:" /* one or more: */ \
      "[^\\s()<>]+" /* run of non-space non-() */ \
      "|" /* or */ \
      BALANCED_PARENS /* balanced parens */ \
    ")+" \
    "(?:" /* end with: */ \
      BALANCED_PARENS /* balanced parens */ \
      "|" /* or */ \
      NOT_TRAILING_JUNK /* last non-junk char */ \
    ")" \
  ")"

G_DEFINE_BOXED_TYPE (ShellUrlMatch, shell_url_match,
                     shell_url_match_copy,
                     shell_url_match_free)

/**
 * shell_url_match_copy:
 * @match: a #ShellUrlMatch
 *
 * Returns: (transfer full): a copy of @match
 */
ShellUrlMatch *
shell_url_match_copy (const ShellUrlMatch *match)
{
  ShellUrlMatch *copy;

  g_return_val_if_fail (match != NULL, NULL);

  copy = g_new0 (ShellUrlMatch, 1);
  copy->url = g_strdup (match->url);
  copy->pos = match->pos;
  copy->byte_pos = match->byte_pos;

  return copy;
}

/**
 * shell_url_match_free:
 * @match: a #ShellUrlMatch
 *
 * Frees @match.
 */
void
shell_url_match_free (ShellUrlMatch *match)
{
  if (match == NULL)
    return;

  g_free (match->url);
  g_free (match);
}

static GRegex *
get_url_regex (void)
{
  static GRegex *regex = NULL;

  if (g_once_init_enter (&regex))
    {
      g_autoptr (GError) error = NULL;
      GRegex *new_regex = g_regex_new (URL_REGEXP,
                                       G_REGEX_CASELESS | G_REGEX_OPTIMIZE,
                                       0, &error);

      if (new_regex == NULL)
        g_error ("Invalid URL regular expression: %s", error->message);

      g_once_init_leave (&regex, new_regex);
    }

  return regex;
}

/**
 * shell_util_find_urls:
 * @str: the string to find URLs in
 *
 * Searches @str for URLs (`findUrls ()` of js/misc/util.js).
 *
 * Returns: (transfer full) (element-type ShellUrlMatch): the URLs found in
 *   @str, in the order of their position
 */
GList *
shell_util_find_urls (const char *str)
{
  g_autoptr (GMatchInfo) info = NULL;
  GList *matches = NULL;
  GRegex *regex;

  g_return_val_if_fail (str != NULL, NULL);

  if (!g_utf8_validate (str, -1, NULL))
    return NULL;

  regex = get_url_regex ();
  g_regex_match (regex, str, 0, &info);

  while (g_match_info_matches (info))
    {
      ShellUrlMatch *match;
      int leading_start, leading_end, url_start, url_end;

      if (!g_match_info_fetch_pos (info, 1, &leading_start, &leading_end) ||
          !g_match_info_fetch_pos (info, 2, &url_start, &url_end))
        break;

      match = g_new0 (ShellUrlMatch, 1);
      match->url = g_strndup (str + url_start, url_end - url_start);
      match->byte_pos = url_start;
      match->pos = g_utf8_pointer_to_offset (str, str + url_start);
      matches = g_list_prepend (matches, match);

      if (!g_match_info_next (info, NULL))
        break;
    }

  return g_list_reverse (matches);
}

/**
 * shell_util_fix_markup:
 * @text: the text containing markup to escape and parse
 * @allow_markup: whether to keep the `b`, `i` and `u` markup, or escape it all
 *
 * Fixes up embedded markup so that it can be displayed correctly in UI
 * elements such as the message list (`fixMarkup ()` of js/misc/util.js). In
 * some cases some of the embedded markup is kept, @allow_markup asks for that.
 *
 * Returns: (transfer full): the escaped string
 */
char *
shell_util_fix_markup (const char *text,
                       gboolean    allow_markup)
{
  static GRegex *amp_regex = NULL;
  static GRegex *lt_regex = NULL;

  g_return_val_if_fail (text != NULL, NULL);

  if (allow_markup)
    {
      g_autofree char *amp_fixed = NULL;
      g_autofree char *fixed = NULL;

      if (g_once_init_enter (&amp_regex))
        {
          g_once_init_leave (&amp_regex,
                             g_regex_new ("&(?!amp;|quot;|apos;|lt;|gt;)",
                                          G_REGEX_OPTIMIZE, 0, NULL));
        }
      if (g_once_init_enter (&lt_regex))
        {
          g_once_init_leave (&lt_regex,
                             g_regex_new ("<(?!/?[biu]>)",
                                          G_REGEX_OPTIMIZE, 0, NULL));
        }

      /* Support &amp;, &quot;, &apos;, &lt; and &gt;, escape all other
       * occurrences of '&'. */
      amp_fixed = g_regex_replace_literal (amp_regex, text, -1, 0, "&amp;", 0, NULL);

      /* Support <b>, <i>, and <u>, escape anything else so it displays as
       * raw markup. */
      if (amp_fixed != NULL)
        fixed = g_regex_replace_literal (lt_regex, amp_fixed, -1, 0, "&lt;", 0, NULL);

      if (fixed != NULL &&
          pango_parse_markup (fixed, -1, 0, NULL, NULL, NULL, NULL))
        return g_steal_pointer (&fixed);
    }

  /* !allow_markup, or invalid markup */
  return g_markup_escape_text (text, -1);
}
