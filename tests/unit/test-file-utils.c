/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Unit tests for the file helpers of the shell: the collection of the files
 * of the data directories (shell_util_collect_from_datadirs (), src/shell-util.c)
 * and the recursive deletion and move of directories
 * (shell_extension_recursively_delete_dir () and
 * shell_extension_recursively_move_dir (), src/ui/extension-system.c). Port of
 * tests/unit/fileUtils.js.
 *
 * The data directories are those of tests/unit/fixtures/datadirs, which the
 * test environment sets in XDG_DATA_HOME and XDG_DATA_DIRS.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <gio/gio.h>

#include "shell-util.h"
#include "ui/extension-system-private.h"

#define SUBDIR "collect-datadirs"

/* collectFromDatadirs () */

static void
test_collect_all_datadirs (void)
{
  g_autoptr (GPtrArray) files = shell_util_collect_from_datadirs (SUBDIR, TRUE);

  g_assert_cmpuint (files->len, ==, 2);
}

static void
test_collect_system_datadirs (void)
{
  g_autoptr (GPtrArray) files = shell_util_collect_from_datadirs (SUBDIR, FALSE);

  g_assert_cmpuint (files->len, ==, 1);
}

static void
test_collect_returns_file_and_info (void)
{
  g_autoptr (GPtrArray) files = shell_util_collect_from_datadirs (SUBDIR, FALSE);
  ShellDatadirChild *child;

  g_assert_cmpuint (files->len, >, 0);
  child = g_ptr_array_index (files, 0);
  g_assert_true (G_IS_FILE (child->file));
  g_assert_true (G_IS_FILE_INFO (child->info));
}

static void
test_collect_user_files_first (void)
{
  g_autoptr (GPtrArray) files = shell_util_collect_from_datadirs (SUBDIR, TRUE);
  const char *data_home = g_get_user_data_dir ();
  ShellDatadirChild *first, *last;
  g_autofree char *first_path = NULL;
  g_autofree char *last_path = NULL;

  g_assert_cmpuint (files->len, ==, 2);
  first = g_ptr_array_index (files, 0);
  last = g_ptr_array_index (files, 1);
  first_path = g_file_get_path (first->file);
  last_path = g_file_get_path (last->file);

  g_assert_true (g_str_has_prefix (first_path, data_home));
  g_assert_false (g_str_has_prefix (last_path, data_home));
}

static void
test_collect_file_info_attributes (void)
{
  g_autoptr (GPtrArray) files = shell_util_collect_from_datadirs (SUBDIR, FALSE);
  ShellDatadirChild *child;

  g_assert_cmpuint (files->len, >, 0);
  child = g_ptr_array_index (files, 0);
  g_assert_true (g_file_info_has_attribute (child->info, "standard::name"));
  g_assert_true (g_file_info_has_attribute (child->info, "standard::type"));
}

/* recursivelyDeleteDir () and recursivelyMoveDir () */

typedef struct
{
  GFile *dir;     /* the directory to delete, the source of the move */
  GFile *other;   /* the destination of the move */
} DirsFixture;

static GFile *
make_tmp_dir (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *path = g_dir_make_tmp ("gnome-shell.unit-fileUtils.XXXXXX", &error);

  g_assert_no_error (error);
  return g_file_new_for_path (path);
}

static void
dirs_setup (DirsFixture *fixture,
            gconstpointer data)
{
  fixture->dir = make_tmp_dir ();
  fixture->other = make_tmp_dir ();
}

static void
dirs_teardown (DirsFixture *fixture,
               gconstpointer data)
{
  g_autoptr (GError) error = NULL;

  /* Whatever the test left */
  if (g_file_query_exists (fixture->other, NULL))
    {
      g_assert_true (shell_extension_recursively_delete_dir (fixture->other, TRUE, &error));
      g_assert_no_error (error);
    }
  if (g_file_query_exists (fixture->dir, NULL))
    {
      g_assert_true (shell_extension_recursively_delete_dir (fixture->dir, TRUE, &error));
      g_assert_no_error (error);
    }

  g_clear_object (&fixture->dir);
  g_clear_object (&fixture->other);
}

static void
touch_file (GFile      *parent,
            const char *name)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) child = g_file_get_child (parent, name);
  g_autoptr (GFileOutputStream) stream = g_file_create (child, G_FILE_CREATE_NONE, NULL, &error);

  g_assert_no_error (error);
  g_assert_true (g_output_stream_close (G_OUTPUT_STREAM (stream), NULL, &error));
  g_assert_no_error (error);
}

static void
make_link (GFile      *parent,
           const char *name)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) link = g_file_get_child (parent, name);

  g_assert_true (g_file_make_symbolic_link (link, "file", NULL, &error));
  g_assert_no_error (error);
}

static gboolean
child_exists (GFile      *parent,
              const char *name)
{
  g_autoptr (GFile) child = g_file_get_child (parent, name);

  return g_file_query_exists (child, NULL);
}

static void
test_delete_contents (DirsFixture *fixture,
                      gconstpointer data)
{
  g_autoptr (GError) error = NULL;

  touch_file (fixture->dir, "file");

  g_assert_true (shell_extension_recursively_delete_dir (fixture->dir, FALSE, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_query_exists (fixture->dir, NULL));
  g_assert_false (child_exists (fixture->dir, "file"));
}

static void
test_delete_parent (DirsFixture *fixture,
                    gconstpointer data)
{
  g_autoptr (GError) error = NULL;

  touch_file (fixture->dir, "file");

  g_assert_true (shell_extension_recursively_delete_dir (fixture->dir, TRUE, &error));
  g_assert_no_error (error);
  g_assert_false (g_file_query_exists (fixture->dir, NULL));
}

static void
test_delete_subdirs (DirsFixture *fixture,
                     gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) subdir = g_file_get_child (fixture->dir, "subdir");

  g_assert_true (g_file_make_directory (subdir, NULL, &error));
  g_assert_no_error (error);
  touch_file (subdir, "file");

  g_assert_true (shell_extension_recursively_delete_dir (fixture->dir, TRUE, &error));
  g_assert_no_error (error);
  g_assert_false (g_file_query_exists (fixture->dir, NULL));
}

static void
test_delete_symlinks (DirsFixture *fixture,
                      gconstpointer data)
{
  g_autoptr (GError) error = NULL;

  touch_file (fixture->dir, "file");
  make_link (fixture->dir, "link");

  g_assert_true (shell_extension_recursively_delete_dir (fixture->dir, TRUE, &error));
  g_assert_no_error (error);
  g_assert_false (g_file_query_exists (fixture->dir, NULL));
}

static void
test_move_contents (DirsFixture *fixture,
                    gconstpointer data)
{
  g_autoptr (GError) error = NULL;

  touch_file (fixture->dir, "file");

  g_assert_true (shell_extension_recursively_move_dir (fixture->dir, fixture->other, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_query_exists (fixture->other, NULL));
  g_assert_true (child_exists (fixture->other, "file"));
}

static void
test_move_creates_destination (DirsFixture *fixture,
                               gconstpointer data)
{
  g_autoptr (GError) error = NULL;

  g_assert_true (g_file_delete (fixture->other, NULL, &error));
  g_assert_no_error (error);
  g_assert_false (g_file_query_exists (fixture->other, NULL));

  g_assert_true (shell_extension_recursively_move_dir (fixture->dir, fixture->other, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_query_exists (fixture->other, NULL));
}

static void
test_move_subdirs (DirsFixture *fixture,
                   gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) subdir = g_file_get_child (fixture->dir, "subdir");
  g_autoptr (GFile) other_subdir = g_file_get_child (fixture->other, "subdir");

  g_assert_true (g_file_make_directory (subdir, NULL, &error));
  g_assert_no_error (error);
  touch_file (subdir, "file");

  g_assert_true (shell_extension_recursively_move_dir (fixture->dir, fixture->other, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_query_exists (fixture->other, NULL));
  g_assert_true (g_file_query_exists (other_subdir, NULL));
  g_assert_true (child_exists (other_subdir, "file"));
}

static void
test_move_symlinks (DirsFixture *fixture,
                    gconstpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) link = NULL;
  g_autoptr (GFileInfo) info = NULL;

  touch_file (fixture->dir, "file");
  make_link (fixture->dir, "link");

  g_assert_true (shell_extension_recursively_move_dir (fixture->dir, fixture->other, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_query_exists (fixture->other, NULL));

  /* The link itself is moved (its target "file" is in the destination too) */
  link = g_file_get_child (fixture->other, "link");
  info = g_file_query_info (link, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);
  g_assert_no_error (error);
  g_assert_cmpint (g_file_info_get_file_type (info), ==, G_FILE_TYPE_SYMBOLIC_LINK);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/shell/file-utils/collect-from-datadirs/all", test_collect_all_datadirs);
  g_test_add_func ("/shell/file-utils/collect-from-datadirs/system-only", test_collect_system_datadirs);
  g_test_add_func ("/shell/file-utils/collect-from-datadirs/file-and-info", test_collect_returns_file_and_info);
  g_test_add_func ("/shell/file-utils/collect-from-datadirs/user-first", test_collect_user_files_first);
  g_test_add_func ("/shell/file-utils/collect-from-datadirs/info-attributes", test_collect_file_info_attributes);

  g_test_add ("/shell/file-utils/recursively-delete-dir/contents", DirsFixture, NULL,
              dirs_setup, test_delete_contents, dirs_teardown);
  g_test_add ("/shell/file-utils/recursively-delete-dir/parent", DirsFixture, NULL,
              dirs_setup, test_delete_parent, dirs_teardown);
  g_test_add ("/shell/file-utils/recursively-delete-dir/subdirs", DirsFixture, NULL,
              dirs_setup, test_delete_subdirs, dirs_teardown);
  g_test_add ("/shell/file-utils/recursively-delete-dir/symlinks", DirsFixture, NULL,
              dirs_setup, test_delete_symlinks, dirs_teardown);

  g_test_add ("/shell/file-utils/recursively-move-dir/contents", DirsFixture, NULL,
              dirs_setup, test_move_contents, dirs_teardown);
  g_test_add ("/shell/file-utils/recursively-move-dir/creates-destination", DirsFixture, NULL,
              dirs_setup, test_move_creates_destination, dirs_teardown);
  g_test_add ("/shell/file-utils/recursively-move-dir/subdirs", DirsFixture, NULL,
              dirs_setup, test_move_subdirs, dirs_teardown);
  g_test_add ("/shell/file-utils/recursively-move-dir/symlinks", DirsFixture, NULL,
              dirs_setup, test_move_symlinks, dirs_teardown);

  return g_test_run ();
}
