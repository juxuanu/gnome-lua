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
