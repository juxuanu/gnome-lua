/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * shell-test-runner.h: Runs a Lua test script inside the shell (private).
 *
 * When the environment variable SHELL_TEST_SCRIPT names a .lua file,
 * shell_test_runner_start () waits for the shell to be built (the objects of
 * ShellMain exist), creates a privileged Lua state like the one of Looking
 * Glass (generic handles with the properties of GObjects, plus the globals
 * `global`, `main` (ShellMain), `stage`, and the `test` module), runs the
 * script in a coroutine, and terminates the shell: successfully if the
 * script and its callbacks finish without an error, with an error (non-zero
 * exit status) otherwise. gnome-shell-test-tool sets the variable.
 *
 * The script is plain Lua, run as a coroutine; the functions of `test`
 * that wait (test.sleep (), test.wait_leisure (), test.wait_startup (),
 * test.wait (), test.create_window () ...) yield it until the main loop has
 * done what was asked. See the head of shell-test-runner.c for the whole
 * API; the Lua files of tests/shell are the examples.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/*
 * Whether SHELL_TEST_SCRIPT is set. The shell does not show the dialogs and
 * notifications that would get in the way of a test (the welcome dialog, the
 * notification for a privileged user) when this is true; they were skipped
 * when an automation script was given.
 */
gboolean shell_test_runner_is_enabled (void);

/*
 * Starts the runner if SHELL_TEST_SCRIPT is set; does nothing otherwise.
 * Call it once, from main (), after the Mutter context was started and
 * before the main loop runs: it attaches itself to the main loop, polls for
 * ShellMain to be ready and starts the script then, before the startup of
 * the shell is complete (a script calls test.wait_startup () to wait for
 * the end of the startup animation).
 */
void     shell_test_runner_start      (void);

G_END_DECLS
