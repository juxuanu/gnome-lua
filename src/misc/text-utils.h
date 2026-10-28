/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * text-utils.h: Text helpers of the message list: finding URLs in a text and
 * fixing up embedded markup. Ports of `findUrls ()` and `fixMarkup ()` of
 * js/misc/util.js, which forward to these functions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define SHELL_TYPE_URL_MATCH (shell_url_match_get_type ())

typedef struct _ShellUrlMatch ShellUrlMatch;

/**
 * ShellUrlMatch:
 * @url: the URL that was found, as it appears in the text
 * @pos: the position of the URL in the text, in characters (the `pos` of the
 *   objects `findUrls ()` returned)
 * @byte_pos: the position of the URL in the text, in bytes
 *
 * A URL found by shell_util_find_urls ().
 */
struct _ShellUrlMatch
{
  char *url;
  int   pos;
  int   byte_pos;
};

GType          shell_url_match_get_type (void) G_GNUC_CONST;
ShellUrlMatch *shell_url_match_copy     (const ShellUrlMatch *match);
void           shell_url_match_free     (ShellUrlMatch       *match);

GList     *shell_util_find_urls (const char *str);

char      *shell_util_fix_markup (const char *text,
                                  gboolean    allow_markup);

G_END_DECLS
