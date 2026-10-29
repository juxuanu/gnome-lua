# Performance

The M6 exit criterion of `docs/PLAN.md` asks for startup time, resident
memory and frame time of the C-only shell against upstream gnome-shell 51.1
on the same machine. This page holds the method and, once measured, the
numbers.

## Status

**Not measured yet.** The port was developed and verified in a headless
container and in CI (a software renderer without a GPU, under
AddressSanitizer), where none of these numbers mean anything. They need one
real machine with a GPU, upstream 51.1 and this tree installed side by side,
and the same session (same extensions: none, same background, same
monitors).

## Method

Three sessions of each shell, the first discarded (caches), the median of
the other two reported.

| Metric | How |
|---|---|
| Startup time | `systemd-analyze --user blame` for `org.gnome.Shell@wayland.service`, and the time from the service start to the `startup-complete` signal of the layout manager (`journalctl --user -u org.gnome.Shell@wayland.service` shows "GNOME Shell started at" from `ShellMain`). |
| Resident memory | `smem -P gnome-shell -c "pss rss"` one minute after login with no window open, and again after opening and closing the overview, the system menu and the calendar five times. |
| Frame time | `gnome-shell --headless`-free: `GNOME_SHELL_SLOWDOWN_FACTOR=1` with `MUTTER_DEBUG_ENABLE_FRAME_TIMINGS=1`, or Sysprof's "Compositor" marks, while switching workspaces ten times and opening the overview ten times; report the 50th and 99th percentile of the frame time. |
| Extension cost | With the `hooks-log@gnome-lua.example` extension enabled, the same measurements; the watchdog counters of each extension (`shell_lua_state_get_stats ()`, shown in Looking Glass) give the time spent in Lua. |

The JS runtime cost that the port removes shows up mostly in the startup
time (no GJS context, no module loading) and in the resident memory (no
SpiderMonkey heap); the frame time should be unchanged, as the compositing
path was C already.

## Results

| Metric | upstream 51.1 | this tree |
|---|---|---|
| Startup time | | |
| RSS after login | | |
| RSS after the menus | | |
| Frame time p50 / p99 | | |
