# Debugging

## Looking Glass

[Looking Glass][lg] is a built-in debugger and inspector tool that makes
it possible to run arbitrary Lua code inside the shell, visually select
any object from the shell's scene, and much more.

It can be accessed by entering `lg` into the run dialog (Alt+F2).

[lg]: looking-glass.md

## Stacktraces

gnome-shell is C with an embedded Lua interpreter for extensions. When it
receives a fatal signal (SIGABRT, SIGFPE, SIGTRAP, and SIGSEGV/SIGBUS with
`SHELL_DEBUG=backtrace-segfaults`), it prints the Lua traceback of the
extension that was running, if any, then the C stack (symbolized by the
sanitizer in AddressSanitizer builds, `backtrace_symbols_fd ()` otherwise).

The `SHELL_DEBUG` environment variable can be used to print the same stacks
automatically:

 * `backtrace-warnings`: when a warning or critical is logged
 * `backtrace-segfaults`: on segfaults

An error in the code of an extension is logged with its Lua traceback by the
extension manager. In the Looking Glass evaluator (which is not sandboxed),
`print (debug.traceback ('trace from do_something ()'))` logs a traceback.

## An extension that makes the desktop stutter

Extensions run on the main thread of the shell, which also draws the
screen: a callback that takes longer than a frame (16 ms at 60 Hz) drops
frames. The runtime measures every call into an extension (callbacks,
hooks, timers, `enable ()`), from its enable on, without the time of other
extensions' callbacks that run inside it:

 * the journal gets a line for a call longer than a frame, at most one every
   10 seconds per extension, with the function it ran:
   `[clock@example.com] a call took 42 ms (extension.lua:57)`
   (`journalctl --user -b _COMM=gnome-shell -g 'a call took'`);
 * `gnome-extensions stats` lists the running extensions, the most expensive
   first: the number of callbacks and their time in total, the longest one
   and where it is defined, the callbacks longer than a frame and the Lua
   memory, its workers' included (`--sort longest`, `slow`, `callbacks` or
   `memory`);
 * `gnome-extensions info UUID` shows the same figures for one extension, and
   the Extensions tab of Looking Glass under each extension (the figures are
   those of when the tab was shown).

The figures are the keys `callbacks`, `callbackTime`, `callbackLongest`
(milliseconds), `callbackLongestWhere`, `slowCallbacks` and `memory` (bytes)
of the extension info (`org.gnome.Shell.Extensions.GetExtensionInfo`);
`ShellLuaStats` in `subprojects/shell-lua/src/shell-lua.h` in C.

An extension with workers (`docs/api/worker.md`) runs code off the main
thread too; that time costs no frames and is shown apart, never added to the
figures above: `gnome-extensions info` prints `Workers` (running, messages
handled, their CPU time in total), `Longest worker message` and `Worker
memory` (the Lua memory of the workers that run now), Looking Glass a line
of its own, and the extension info carries `workers`, `workerMessages`,
`workerTime` and `workerLongest` (milliseconds) and `workerMemory` (bytes;
`ShellLuaWorkerStats` in `shell-lua-extension.h`). What a worker does cost the main thread, the
conversion of its messages and the `on_message` callbacks, is in the callback
figures like any other callback.

## Debugging the session's gnome-shell proces

It is possible to attach gdb to the gnome-shell process of the existing
login session (or with the native backend on a tty), but beware: When the
debugger stops the gnome-shell process, the entire session will freeze.

It is necessary to use a second system to continue controlling the
debugger, most conveniently in combination with [screen]:

 1. create a screen session
 2. detach the session
 3. ssh from a second system
 4. resume the screen session
 5. attach gdb to the gnome-shell process

[screen]: https://www.gnu.org/software/screen/
