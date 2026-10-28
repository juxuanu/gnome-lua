/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * extension-prefs-schema.h: Loads and validates the declarative preferences
 * of a Lua extension (`prefs.lua`, see docs/api/prefs.md).
 *
 * The file is run once in a sandboxed Lua state that only holds
 * `shell.api_version`, `shell.extension`, `shell.log`, `shell.gettext` and the
 * globals `_`, `ngettext` and `pgettext`; the table it returns is converted to
 * plain data, validated against the GSettings schema of the extension and
 * turned into the structures below, which the window of the preferences
 * service renders. No Lua value outlives the call.
 *
 * This file only depends on GLib, json-glib and libshell-lua, so that the
 * validation can be tested without a display and reused by tools.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

#include "shell-lua-extension.h"

G_BEGIN_DECLS

typedef enum
{
  EXT_PREFS_ROW_SWITCH,
  EXT_PREFS_ROW_SPIN,
  EXT_PREFS_ROW_COMBO,
  EXT_PREFS_ROW_ENTRY,
  EXT_PREFS_ROW_COLOR,
  EXT_PREFS_ROW_KEYBINDING,
} ExtPrefsRowType;

typedef struct
{
  char     *label;
  GVariant *value;     /* of the type of the key */
} ExtPrefsChoice;

typedef struct
{
  char     *key;
  GVariant *value;     /* the row is active while the key equals this */
} ExtPrefsDepends;

typedef struct
{
  ExtPrefsRowType  type;
  char            *key;
  char            *title;
  char            *subtitle;           /* nullable */
  ExtPrefsDepends *depends_on;         /* nullable */

  /* EXT_PREFS_ROW_SPIN */
  double           min;
  double           max;
  double           step;
  unsigned int     digits;

  /* EXT_PREFS_ROW_COMBO: ExtPrefsChoice* */
  GPtrArray       *choices;

  /* EXT_PREFS_ROW_ENTRY: 0 for no limit */
  unsigned int     max_length;

  /* EXT_PREFS_ROW_COLOR */
  gboolean         alpha;

  /* EXT_PREFS_ROW_KEYBINDING */
  unsigned int     max_accels;
  gboolean         allow_empty;
} ExtPrefsRow;

typedef struct
{
  char      *title;         /* nullable */
  char      *description;   /* nullable */
  GPtrArray *rows;          /* ExtPrefsRow* */
} ExtPrefsGroup;

typedef struct
{
  char      *title;         /* nullable only for a single page */
  char      *icon_name;     /* nullable */
  GPtrArray *groups;        /* ExtPrefsGroup* */
} ExtPrefsPage;

typedef struct
{
  char      *title;         /* nullable: the name of the extension */
  GPtrArray *pages;         /* ExtPrefsPage* */
  GSettings *settings;      /* the settings of the extension */
} ExtPrefsSchema;

ExtPrefsSchema *ext_prefs_schema_load (ShellLuaExtension  *extension,
                                       char             ***problems);
void            ext_prefs_schema_free (ExtPrefsSchema     *schema);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ExtPrefsSchema, ext_prefs_schema_free)

G_END_DECLS
