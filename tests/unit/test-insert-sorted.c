/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Unit tests for shell_util_insert_sorted () (src/shell-util.c), which
 * inserts a value into a sorted array. Port of tests/unit/insertSorted.js.
 * The JS function sorted numbers by default; the C one always takes the
 * comparison function.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib.h>

#include "shell-util.h"

static int
compare_ints (gconstpointer one,
              gconstpointer two)
{
  return GPOINTER_TO_INT (one) - GPOINTER_TO_INT (two);
}

static int
compare_ints_descending (gconstpointer one,
                         gconstpointer two)
{
  return GPOINTER_TO_INT (two) - GPOINTER_TO_INT (one);
}

static void
assert_ints (GPtrArray *array,
             const int *expected,
             guint      n_expected)
{
  guint i;

  g_assert_cmpuint (array->len, ==, n_expected);
  for (i = 0; i < n_expected; i++)
    g_assert_cmpint (GPOINTER_TO_INT (g_ptr_array_index (array, i)), ==, expected[i]);
}

static GPtrArray *
new_int_array (const int *values,
               guint      n_values)
{
  GPtrArray *array = g_ptr_array_new ();
  guint i;

  for (i = 0; i < n_values; i++)
    g_ptr_array_add (array, GINT_TO_POINTER (values[i]));

  return array;
}

static void
test_integer_sorting (void)
{
  const int values[] = { 1, 2, 3, 5, 6 };
  const int expected[] = { 1, 2, 3, 4, 5, 6 };
  g_autoptr (GPtrArray) array = new_int_array (values, G_N_ELEMENTS (values));

  g_assert_cmpuint (shell_util_insert_sorted (array, GINT_TO_POINTER (4), compare_ints), ==, 3);
  assert_ints (array, expected, G_N_ELEMENTS (expected));
}

static void
test_custom_compare_function (void)
{
  const int values[] = { 6, 5, 3, 2, 1 };
  const int expected[] = { 6, 5, 4, 3, 2, 1 };
  g_autoptr (GPtrArray) array = new_int_array (values, G_N_ELEMENTS (values));

  shell_util_insert_sorted (array, GINT_TO_POINTER (4), compare_ints_descending);
  assert_ints (array, expected, G_N_ELEMENTS (expected));
}

typedef struct
{
  int a;
  int b;
} Object;

static int
compare_objects (gconstpointer one,
                 gconstpointer two)
{
  const Object *object1 = one;
  const Object *object2 = two;

  return object1->a - object2->a;
}

static void
test_inserts_before_first_equal_match (void)
{
  Object obj1 = { 1, 0 };
  Object obj2 = { 2, 0 };
  Object obj3 = { 2, 1 };
  Object obj4 = { 3, 0 };
  g_autoptr (GPtrArray) array = g_ptr_array_new ();

  g_ptr_array_add (array, &obj1);
  g_ptr_array_add (array, &obj3);
  g_ptr_array_add (array, &obj4);

  g_assert_cmpuint (shell_util_insert_sorted (array, &obj2, compare_objects), ==, 1);

  g_assert_cmpuint (array->len, ==, 4);
  g_assert_true (g_ptr_array_index (array, 0) == &obj1);
  g_assert_true (g_ptr_array_index (array, 1) == &obj2);
  g_assert_true (g_ptr_array_index (array, 2) == &obj3);
  g_assert_true (g_ptr_array_index (array, 3) == &obj4);
}

static int n_compare_calls = 0;

static int
counting_compare (gconstpointer one,
                  gconstpointer two)
{
  n_compare_calls++;
  return 0;
}

static void
test_empty_array_does_not_compare (void)
{
  g_autoptr (GPtrArray) empty = g_ptr_array_new ();

  n_compare_calls = 0;
  shell_util_insert_sorted (empty, GINT_TO_POINTER (3), counting_compare);

  g_assert_cmpint (n_compare_calls, ==, 0);
  g_assert_cmpuint (empty->len, ==, 1);
}

/* Only compares elements of the array and the value to insert: a comparison
 * past the bounds would see a NULL */
static int
checked_compare (gconstpointer one,
                 gconstpointer two)
{
  g_assert_nonnull (one);
  g_assert_nonnull (two);

  return GPOINTER_TO_INT (one) - GPOINTER_TO_INT (two);
}

static void
test_does_not_access_past_bounds_at_end (void)
{
  const int values[] = { 3 };
  const int expected[] = { 3, 4, 5 };
  g_autoptr (GPtrArray) array = new_int_array (values, G_N_ELEMENTS (values));

  shell_util_insert_sorted (array, GINT_TO_POINTER (4), checked_compare);
  shell_util_insert_sorted (array, GINT_TO_POINTER (5), checked_compare);
  assert_ints (array, expected, G_N_ELEMENTS (expected));
}

static void
test_does_not_access_past_bounds_at_beginning (void)
{
  const int values[] = { 4, 5 };
  const int expected[] = { 1, 2, 4, 5 };
  g_autoptr (GPtrArray) array = new_int_array (values, G_N_ELEMENTS (values));

  shell_util_insert_sorted (array, GINT_TO_POINTER (1), checked_compare);
  shell_util_insert_sorted (array, GINT_TO_POINTER (2), checked_compare);
  assert_ints (array, expected, G_N_ELEMENTS (expected));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/shell/util/insert-sorted/integer-sorting", test_integer_sorting);
  g_test_add_func ("/shell/util/insert-sorted/custom-compare-function", test_custom_compare_function);
  g_test_add_func ("/shell/util/insert-sorted/before-first-equal-match", test_inserts_before_first_equal_match);
  g_test_add_func ("/shell/util/insert-sorted/empty-array-does-not-compare", test_empty_array_does_not_compare);
  g_test_add_func ("/shell/util/insert-sorted/past-bounds-at-end", test_does_not_access_past_bounds_at_end);
  g_test_add_func ("/shell/util/insert-sorted/past-bounds-at-beginning", test_does_not_access_past_bounds_at_beginning);

  return g_test_run ();
}
