/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Unit tests for shell_util_gnome_version_compare () (src/shell-util.c), the
 * comparison of GNOME versions. Port of tests/unit/versionCompare.js.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib.h>

#include "shell-util.h"

static void
test_matching_versions (void)
{
  g_assert_cmpint (shell_util_gnome_version_compare ("40", "40"), ==, 0);
}

static void
test_older_versions (void)
{
  g_assert_cmpint (shell_util_gnome_version_compare ("40", "42"), <, 0);
  g_assert_cmpint (shell_util_gnome_version_compare ("40.0", "40.1"), <, 0);
}

static void
test_newer_versions (void)
{
  g_assert_cmpint (shell_util_gnome_version_compare ("42", "40"), >, 0);
  g_assert_cmpint (shell_util_gnome_version_compare ("40.1", "40.0"), >, 0);
}

static void
test_legacy_versions (void)
{
  g_assert_cmpint (shell_util_gnome_version_compare ("3.38.0", "40"), <, 0);
  g_assert_cmpint (shell_util_gnome_version_compare ("40", "3.38.0"), >, 0);
}

static void
test_pre_release_versions (void)
{
  g_assert_cmpint (shell_util_gnome_version_compare ("40.beta", "40"), <, 0);
  g_assert_cmpint (shell_util_gnome_version_compare ("40.alpha", "40"), <, 0);
  g_assert_cmpint (shell_util_gnome_version_compare ("40.rc", "40"), <, 0);

  g_assert_cmpint (shell_util_gnome_version_compare ("40", "40.beta"), >, 0);
  g_assert_cmpint (shell_util_gnome_version_compare ("40", "40.alpha"), >, 0);
  g_assert_cmpint (shell_util_gnome_version_compare ("40", "40.rc"), >, 0);

  g_assert_cmpint (shell_util_gnome_version_compare ("40.alpha", "40.beta"), <, 0);
  g_assert_cmpint (shell_util_gnome_version_compare ("40.beta", "40.rc"), <, 0);
  g_assert_cmpint (shell_util_gnome_version_compare ("40.rc", "40.0"), <, 0);
}

static void
test_point_release_versions (void)
{
  g_assert_cmpint (shell_util_gnome_version_compare ("40.1", "40"), >, 0);
  g_assert_cmpint (shell_util_gnome_version_compare ("40.alpha.1", "40.alpha"), >, 0);
  g_assert_cmpint (shell_util_gnome_version_compare ("40.beta.1.1", "40.beta.1"), >, 0);

  g_assert_cmpint (shell_util_gnome_version_compare ("40", "40.1"), <, 0);
  g_assert_cmpint (shell_util_gnome_version_compare ("40.alpha", "40.alpha.1"), <, 0);
  g_assert_cmpint (shell_util_gnome_version_compare ("40.beta.1", "40.beta.1.1"), <, 0);
}

static void
test_empty_versions (void)
{
  g_assert_cmpint (shell_util_gnome_version_compare ("", "40"), <, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/shell/util/gnome-version-compare/matching", test_matching_versions);
  g_test_add_func ("/shell/util/gnome-version-compare/older", test_older_versions);
  g_test_add_func ("/shell/util/gnome-version-compare/newer", test_newer_versions);
  g_test_add_func ("/shell/util/gnome-version-compare/legacy", test_legacy_versions);
  g_test_add_func ("/shell/util/gnome-version-compare/pre-release", test_pre_release_versions);
  g_test_add_func ("/shell/util/gnome-version-compare/point-release", test_point_release_versions);
  g_test_add_func ("/shell/util/gnome-version-compare/empty", test_empty_versions);

  return g_test_run ();
}
