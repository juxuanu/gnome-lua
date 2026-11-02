# Performance

The M6 exit criterion of `docs/PLAN.md` asks for startup time, resident
memory and frame time of the C-only shell against upstream gnome-shell 51.1
on the same machine. This page holds the method and, once measured, the
numbers.

## Status

**Measured in a container, not yet on a real machine.** The container
numbers (below) compare both shells built the same way and run headless on
a real GPU, without AddressSanitizer; they hold for what the shell itself
does, but no system service answers there, so a real login has higher
absolute figures. The real-machine numbers still need one machine with
upstream 51.1 and this tree installed side by side, and the same session
(same extensions: none, same background, same monitors).

## Container results

Measured on 2026-11-02 at commit 5bb2154ad, against upstream at 33da06854
(the 51.1 commit in this repository's history).

**Setup.** Both shells built in one Fedora 45 container (Mutter 51.1,
GLib 2.89.4, GJS 1.89.90 with mozjs 140.13) with the same options
(`--buildtype=debugoptimized`, tests off), and started with
`--headless --virtual-monitor 1920x1080 --force-animations --unsafe-mode` on
the host's AMD GPU (amdgpu; Ryzen 7 7800X3D), with private session and
system buses, in-memory GSettings and no extensions. Runs alternate between
the two shells: six each, the first dropped (caches), the median of the
other five reported, with the range where it matters. Memory is read from
`/proc/<pid>/smaps_rollup` and `status`, CPU time from `/proc/<pid>/stat`.

### The shell

The workload is driven over D-Bus: the overview shown and hidden ten times
(`OverviewActive`, one second apart), then the app grid shown
(`ShowApplications`) and hidden five times.

| Metric | upstream 51.1 | this tree | Change |
|---|---|---|---|
| Startup to `READY=1` (wall clock) | 0.322 s (0.320–0.324) | 0.174 s (0.172–0.182) | −46% |
| CPU time to `READY=1` | 0.32 s | 0.17 s | −47% |
| PSS, 5 s after startup | 158.6 MiB | 110.0 MiB | −31% |
| … anonymous | 94.4 MiB | 57.7 MiB | −39% |
| … file-backed | 64.8 MiB | 52.2 MiB | −19% |
| RSS, 5 s after startup | 222.0 MiB | 162.7 MiB | −27% |
| PSS after the workload | 155.9 MiB | 110.5 MiB | −29% |
| Peak RSS | 221.1 MiB | 163.3 MiB | −26% |
| PSS after every 10 of 60 overview cycles | 154.6–158.6 MiB | 109.0–109.9 MiB | flat in both |
| Idle CPU | 0.2% | 0.0% | |
| Threads | 39 | 33 | |
| CPU time, overview shown and hidden 10 times | 0.54 s | 0.34 s | −37% |
| CPU time, app grid shown and hidden 5 times | 0.30 s | 0.20 s | −33% |
| Stripped binary and private libraries | 4.5 MiB, plus 59.9 MiB of libgjs, libmozjs and ICU | 6.2 MiB, of which 0.6 MiB is the Lua runtime | |

Upstream's perf script (`tests/shell/core.js`, and its port
`tests/shell/core.lua`, through
`gnome-shell-test-tool --perf-warmup --test-iters 5`), medians of the five
iterations:

| Metric | upstream 51.1 | this tree | Change |
|---|---|---|---|
| `overviewLatencyFirst` | 12.4 ms | 9.0 ms | −27% |
| `overviewLatencySubsequent` | 15.9 ms | 9.9 ms | −37% |
| `overviewFps*` (5 or 10 windows, maximized) | 59–63 fps | 59–63 fps | same |
| `overviewFps10Alpha` (10 translucent windows) | 5.4 fps | 5.4 fps | same |
| `applicationsShowTimeFirst` / `Subsequent` | 272 / 267 ms | 271 / 267 ms | same |
| `usedAfterOverview` | 63.0 MB | 45.2 MB | −28% |
| `leakedAfterOverview` | −4.0 MB | +1.0 MB | |

- The frame rates are the 60 Hz of the virtual monitor in both shells, and
  the 5.4 fps with translucent windows is the renderer's, not the shell's;
  the app grid times are the length of its animation.
- `usedAfterOverview` counts malloc only, not the SpiderMonkey heap: the PSS
  rows above are the comparable memory figure.
- `leakedAfterOverview` of this tree is a fill between the first and the
  second overview that does not repeat (the 60-cycle row is flat); upstream's
  is negative because its garbage collector returns memory during the test.

### Extensions

The same extensions written for both shells: the indicator is
`subprojects/shell-lua/examples/indicator@gnome-lua.example` (an icon and a
label in the top bar, a menu with an item, a switch, a submenu of three
items, a separator and an image item) and its JS twin built from the same
widgets with `PanelMenu.Button` and `PopupMenu`. Each extension is installed
before startup and enabled with `org.gnome.Shell.Extensions.EnableExtension`.

| Test | upstream 51.1 (JS) | this tree (Lua) |
|---|---|---|
| An endless loop in `enable()` | The shell, and with it the compositor, stops answering within 1 s and has not answered again after 10 s, at 100% CPU. | The watchdog stops it after 500 ms; the extension is in state ERROR, with a traceback as its error; the shell answers in 3 ms throughout. |
| A timer that keeps 500000 more numbers every 100 ms | RSS from 221 to 680 MiB in 12 s and still growing; the extension stays enabled. | The allocation fails at the 64 MiB limit after about 1.3 s (`not enough memory`), the timer that raised does not repeat, RSS stays at 224 MiB. |
| Bytes allocated per extension: an empty one | ~0 | 45 KB, its Lua state |
| … an indicator with an icon | 47 KB | 108–174 KB |
| … the same with five menu items | 234 KB | 335 KB |
| … the indicator example | 600 KB | 620 KB |
| PSS growth for ten copies of the indicator example | 1.6–2.7 MiB | 4.3–5.0 MiB |
| CPU time per enable of the indicator, panel relayout included | 22 ms | 17 ms |

- Bytes allocated are glibc's `mallinfo2 ()` (in use plus mmapped chunks)
  before and after enabling ten copies, with a forced garbage collection
  first in upstream (`imports.system.gc ()` through `Eval`). They include
  the Lua heap, whose allocator is glibc's, but not the JS objects of an
  upstream extension, which live in the SpiderMonkey heap.
- A menu item costs 35–40 KB in both shells: those are the same St widgets.
  The Lua runtime's own share is the state, 45 KB for an empty extension and
  61 KB for the indicator example.
- The PSS of the upstream process grows less than its allocations because
  its heap has room left by the garbage freed after startup, which the new
  widgets take before any new page is touched; this tree's heap starts with
  less slack. Allocated bytes are the comparable figure.

## Method on a real machine

Three sessions of each shell, the first discarded (caches), the median of
the other two reported.

| Metric | How |
|---|---|
| Startup time | `systemd-analyze --user blame` for `org.gnome.Shell@user.service`, and the time from the service start to the `startup-complete` signal of the layout manager (`journalctl --user -u org.gnome.Shell@user.service` shows "GNOME Shell started at" from `ShellMain`). |
| Resident memory | `smem -P gnome-shell -c "pss rss"` one minute after login with no window open, and again after opening and closing the overview, the system menu and the calendar five times. |
| Frame time | Sysprof (`sysprof-cli --gnome-shell`), reading the compositor's frame marks while switching workspaces ten times and opening the overview ten times; report the 50th and 99th percentile of the frame time. |
| Extension cost | With the `hooks-log@gnome-lua.example` extension enabled, the same measurements; the memory of each extension's state (`shell_lua_state_get_memory_usage ()`, shown in Looking Glass) gives its share of the heap. |

The JS runtime cost that the port removes shows up mostly in the startup
time (no GJS context, no module loading) and in the resident memory (no
SpiderMonkey heap); the frame time should be unchanged, as the compositing
path was C already.

## Results on a real machine

| Metric | upstream 51.1 | this tree |
|---|---|---|
| Startup time | | |
| RSS after login | | |
| RSS after the menus | | |
| Frame time p50 / p99 | | |
