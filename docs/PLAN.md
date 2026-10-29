# gnome-lua: project plan

**Objective.** Remove the JavaScript engine (GJS / SpiderMonkey) from GNOME Shell,
embed Lua 5.5 directly in the shell binary, and expose a stable, versioned,
hand-written API for extensions: top-bar indicators, quick-settings entries,
dash (dock) buttons, and lifecycle hooks on common shell actions.

**Interpretation this plan commits to.** Lua is embedded *without* GObject
Introspection (GI). That rules out moving the shell's own UI code from JS to
Lua: that code drives ~45 GI namespaces (Clutter, St, Meta, Gio, GLib, Pango,
NM, …), and hand-binding that surface is GI rewritten by hand. So:

* the shell's UI logic (today 84.6k lines of JS) is ported into the existing
  C core, module by module;
* Lua is the *extension* language, talking to the shell only through a small
  curated C API (`shell.*`), never through raw GObject access;
* GJS is deleted once the last JS module is gone.

If the intent was instead "shell UI in Lua, bound by hand", stop and say so
before M2 starts; everything in M0–M1 is shared by both readings.

## 1. Baseline (gnome-shell 51.1, mutter API 51)

Measured on the upstream tree on 2026-10-28.

| Area | Size |
|---|---:|
| JS to retire (`js/`) | 84,575 lines / 174 modules |
| of which `js/ui` (+ `status/`, `components/`) | 56,603 |
| `js/misc` | 9,075 |
| `js/gdm` (login screen) | 6,922 |
| `js/dbusServices` + `js/extensions` + portal helper | ~1,000 |
| Existing C core (`src/`) | 84,779 lines, of which St toolkit 57,935 |

GJS coupling in C is small and contained:

* `src/shell-global.c` owns the `GjsContext` (created with a `resource:///org/gnome/shell` search path).
* `src/main.c` evaluates `ui/init.js` (which runs the Mutter main loop), `ui/listModes.js`, wires the GJS profiler to Sysprof and dumps JS stacks on fatal signals.
* `src/gnome-shell-plugin.c` only includes `gjs.h`; `src/run-js-test.c` and `src/gnome-shell-portal-helper.c` spin up their own contexts.
* `ShellMemoryInfo` carries four `gjs_*` counters for Looking Glass.
* `meson.build`: `gjs-1.0 >= 1.87.1` and the `gjs` program (used by tests and `dbusServices`).

What the JS layer actually does, which the C port has to absorb:

| JS feature | Count | C replacement |
|---|---:|---|
| `GObject.registerClass` (dynamic GTypes) | 379 | ordinary `G_DECLARE_*_TYPE` classes |
| virtual method overrides (45 distinct; `allocate`, `get_preferred_*`, `style_changed`, `map/unmap`, `paint`, `key_focus_*` dominate) | 202 | class-struct vfuncs |
| classes with custom signals / properties | 70 / 70 (198 `ParamSpec`s) | `g_signal_new`, `g_param_spec_*` |
| `async`/`await` + `_promisify` | 71 files / 61 sites | `GTask`/`GAsyncReadyCallback` (the C idiom these were wrapping) |
| D-Bus proxies & exported objects (`makeProxyWrapper`, `DBusExportedObject`) | 156 | `gdbus-codegen` from the XML already in `data/dbus-interfaces/` |
| `GLib.Variant` construction | 123 | `g_variant_new`, builders |
| `Signals.EventEmitter` plain-JS objects (ExtensionManager, SessionMode, …) | 70 files | become GObjects |
| `environment.js` monkeypatches (`ease`, `connectObject`, `String.format`, spawn wrappers) | 1 file | `ShellActorEase` helpers in C; `connectObject` → `g_signal_connect_object` |

## 2. Decisions

| # | Decision | Why / consequences |
|---|---|---|
| D1 | **Lua 5.5 (PUC), vendored as a Meson wrap subproject**, compiled into the shell. No LuaJIT. | "Latest Lua" (5.5.1, 2026-07). Distros do not ship 5.5 yet, so vendor it; JIT is irrelevant because the hot paths (layout, paint, compositing) are C. Native 64-bit integers, `utf8` lib, incremental/generational GC. |
| D2 | **No GI binding of any kind in the Lua layer.** Lua sees only `shell.*`, implemented as `lua_CFunction`s over opaque userdata handles. | User direction. Keeps the extension API decoupled from internals, which is what makes it stable. |
| D3 | **Shell UI logic moves to C inside `libshell`**, one C module per JS module, same base name (`js/ui/panel.js` → `src/ui/panel.c`). | Keeps upstream diffs mappable during the multi-release port. C is what St, Shell* and Mutter are already written in. Vala and Rust were considered as port accelerators and rejected: Vala adds a toolchain and still generates GObject C; Rust would need `gtk-rs`-style bindings for Clutter/Meta/St that do not exist. |
| D4 | **Strangler-fig migration, JS→C, leaf-first.** Each ported C module is GI-annotated (as all of `src/` already is) so the remaining JS keeps calling it; its JS file is deleted in the same commit. | The shell stays daily-drivable at every commit. GI stays in the *build* only as long as JS exists (M6 removes `g-ir-scanner` for libshell). |
| D5 | **One Lua state per extension**, created from a template state; a shared "shell" state is not used. | Isolation: one extension's error, leak or runaway loop cannot corrupt another's. Cost: one `lua_State` (~tens of KB) per extension, acceptable. |
| D6 | **Sandbox by construction.** Extension `_ENV` gets `string`, `table`, `math`, `utf8`, `coroutine`, `pairs`/`ipairs`/`select`/… and `shell`. No `os`, `io`, `debug`, `load`, `require` of arbitrary paths, no C modules. Capabilities beyond the base (`dbus`, `fs`, `spawn`, `network`) must be declared in `metadata.json` and are shown to the user. | Extensions today are unrestricted; the new API is the moment to fix that. |
| D7 | **Watchdog**: `lua_sethook` count hook plus a wall-clock budget per callback; an extension that exceeds it is disabled and reported. | A frozen shell is the worst extension failure mode today. |
| D8 | **Automatic teardown.** Everything an extension creates through `shell.*` (actors, menu items, hooks, timers, D-Bus names) is owned by its extension context and destroyed on disable. `disable()` is optional. | Removes the largest class of extension bugs (leaked signal handlers / actors). |
| D9 | **Error policy**: every Lua callback runs under `lua_pcall` with a traceback message handler; errors are logged with the extension id, counted, and after N errors the extension is disabled (same policy the JS `ExtensionManager` has today). The shell itself never crashes because of Lua. | |
| D10 | **API versioning**: `shell.api_version = {major=1, minor=n}`; `metadata.json` declares `"shell-api": "1"`. Minor versions are additive only; removals go through a two-release deprecation (runtime warning in Looking Glass and the extensions app). A conformance test suite under `tests/api/` runs against every release. | Replaces today's `shell-version` whitelist, which is exactly what breaks every six months. |
| D11 | **Keep every external contract**: binary name `gnome-shell`, `org.gnome.Shell*` D-Bus interfaces (including `org.gnome.Shell.Extensions` so `gnome-extensions` and the Extensions app keep working), GSettings schemas, session modes, GDM integration, theme CSS. | gnome-session, gsd, gnome-control-center, GDM and distros depend on them. |
| D12 | **Extension preferences are declarative.** `prefs.lua` returns a schema (groups, rows, types bound to GSettings keys); the shell's extensions D-Bus service renders it with Adw in the prefs process. Custom GTK UIs are not exposed. | Removes the need for any GTK binding in Lua; prefs become themeable and consistent. |
| D13 | **Extension packaging stays as is**: a directory/zip with `metadata.json`, `extension.lua`, optional `prefs.lua`, `stylesheet.css`, `schemas/`, `locale/`. JS extensions are not supported, there is no shim. | A compat shim would need GI. Document the migration instead (§7). |
| D14 | **Looking Glass becomes a Lua REPL** against a privileged inspector state (not sandboxed) with `shell.*` plus introspection helpers. | Debugging parity with today. |
| D15 | Licensing: GPL-2.0-or-later (gnome-shell); Lua is MIT and compatible. | |

## 3. Architecture after the port

```
 gnome-shell (C) ────────────────────────────────────────────────────────────
 │ Mutter plugin, ShellGlobal, St toolkit, src/ui/*.c (ported shell UI)
 │
 │  ShellExtensionManager (C)            ShellHookRegistry (C)
 │   ├─ loads metadata.json               named hook points emitted by src/ui/*.c
 │   ├─ one lua_State per extension        (before / after / around, cancellable)
 │   ├─ sandbox + watchdog + teardown
 │   └─ shell.* API  (subprojects/shell-lua + src/lua-api)  ←─ opaque userdata handles, no GI
 │
 └─ Lua 5.5 (vendored subproject)
      extension.lua ─ sandboxed _ENV ─ shell.panel / shell.quick_settings /
                                       shell.dash / shell.hooks / shell.ui / …
```

The runtime lives in `subprojects/shell-lua/`, an in-tree Meson subproject
(the same pattern the repo uses for `gvc`). It depends only on GLib, GIO,
json-glib and the vendored Lua, so it builds and runs its tests standalone on
any distro, and exposes a small `ShellLua-1` GIR so the *remaining JS* can emit
hooks and drive the Looking Glass Lua tab during the port.

```
subprojects/lua.wrap                       wrap-redirect to the nested wrap below
subprojects/shell-lua/subprojects/lua.wrap vendored Lua 5.5.1 + packagefiles meson.build
subprojects/shell-lua/src/
  shell-lua.[ch]              ShellLuaState: lua_State lifecycle, sandbox _ENV, require()
                              over GFile roots, pcall + traceback, watchdog, memory limit
  shell-lua-api.c             API module registry, option-table helpers, call_ref
  shell-lua-handle.[ch]       GObject <-> userdata, weak, curated methods/properties
  shell-lua-variant.[ch]      GVariant <-> Lua (docs/api/variant.md)
  shell-hooks.[ch]            ShellHookRegistry: declare / connect / emit_before / emit_after
  shell-lua-extension*.[ch]   ShellLuaExtension (context, ownership, error policy) + manager
  shell-lua-inspector.[ch]    privileged state for Looking Glass and tests
  shell-lua-api-core.c        shell.log, shell.timer, shell.settings, shell.gettext, shell.extension
  shell-lua-api-hooks.c       shell.hooks
  (M7) shell-lua-api-ui.c, -panel.c, -menu.c, -quick-settings.c, -dash.c,
       -notify.c, -dbus.c, -window.c, -prefs.c
subprojects/shell-lua/tests/  GTest programs, run by `meson test` standalone and in-tree
subprojects/shell-lua/examples/hello@gnome-lua.example/
```

The M7 modules that touch St/Clutter/Meta cannot live in the subproject (it
must stay GLib-only); they go in `src/lua-api/` inside libshell and register
themselves through the same `shell_lua_api_register()` mechanism.

## 4. Milestones

Effort figures are relative (1 = M1). Ordering is by dependency; M2–M5 can run
in parallel across people once M1's conventions exist.

### M0 — Bootstrap (effort 0.5)
* Import gnome-shell 51.1 into this repo as a fork (plain history import, not a
  submodule: `src/`, `data/`, `js/`, `po/`, `tests/` are all modified). Keep an
  `upstream` remote; merge upstream once per GNOME cycle, mutter pinned to the
  matching API version.
* CI: container build (Fedora Rawhide + Arch) with mutter; headless run of
  `tests/shell/*` under `meta-context --headless`; `gnome-shell --nested` smoke test.
* Decide and document C conventions for `src/ui/`: `G_DECLARE_FINAL_TYPE`,
  `g_autoptr`, no new global singletons beyond what `ShellGlobal` already exposes
  (the JS `Main.*` singletons become `shell_global_get_*()` accessors), one
  header per module, GI annotations mandatory while JS remains.
* Exit: CI green on an unmodified fork.

### M1 — Lua embedding (effort 1)
* Vendor Lua 5.5.1 as a Meson wrap; build as a static library into `libshell`.
* `subprojects/shell-lua/src/shell-lua.c`: template state, per-extension state cloning
  (`lua_newstate` + preload of the sandboxed stdlib), `require` searcher limited
  to the extension's own directory and to `resource:///org/gnome/shell/lua/`,
  pcall/traceback wrapper, instruction + time watchdog, memory accounting
  (replaces the `gjs_*` fields in `ShellMemoryInfo`).
* Handle registry: GObject ↔ userdata with weak Lua side; `destroy`/`dispose`
  invalidates the handle so Lua gets a clean error instead of a use-after-free.
* `shell.log`, `shell.timer`, `shell.settings` (GSettings over extension schemas),
  `shell.gettext`.
* `ShellHookRegistry`: `shell_hooks_emit(name, GVariant *payload)` returning a
  verdict; before/after/around semantics; listeners ordered by registration.
* Looking Glass gains a Lua REPL tab (the JS one stays until M4 ports Looking Glass).
* Tests: `subprojects/shell-lua/tests/` GTest programs (no mutter needed),
  including sandbox escape attempts, watchdog, teardown.
* Exit: a sample extension (`examples/hello@gnome-lua.example/`) loads, logs, times, and is torn down
  cleanly, with GJS still running everything else.

### M2 — Core UI to C (effort 3)
Scope: 57 modules, 20.8k JS lines (see `PORT_STATUS.md`): `main`, `sessionMode`,
`layout`, `panel`/`panelMenu`, `popupMenu`, `boxpointer`, `background`, `dnd`,
`grabHelper`, `modalDialog`/`dialog`, `shellDBus`, `osdWindow`, `environment`
helpers, and all of `js/misc`.
* `environment.js` first: `ease()` becomes `shell_actor_ease()` /
  `st_adjustment_ease()` in C (Clutter transitions + completion callback), used by
  both the remaining JS and new C.
* `main.js` becomes `ShellMain` (startup sequencing, session modes, modal stack,
  theme loading); `global` grows accessors for the former `Main.*` singletons.
* Panel and popup menus are ported here because the extension API in M7 attaches
  to them; the panel's hook points (`panel.indicator-added`, `panel.menu-opened`)
  are emitted from the C code from day one.
* Exit: nested session boots with `ui/init.js` reduced to importing the still-JS
  modules; no regression in `tests/shell/*`.

### M3 — Window management and overview to C (effort 3.5)
26 modules, 20k lines: `windowManager`, `workspace*`, `overview*`, `altTab`,
`switcherPopup`, `swipeTracker`, `iconGrid`, `appDisplay`, `dash`, `search*`,
`windowPreview`, `runDialog`, `ctrlAltTab`.
* Heaviest vfunc users (`allocate`, `get_preferred_*`) live here; porting them to
  C is also where measurable frame-time wins are expected.
* Hook points: `overview.showing/shown/hiding/hidden`, `window.mapped/unmapped/
  focused/minimized`, `workspace.switched`, `search.query`, `dash.item-added`.
* Exit: overview, app grid, search, window switching and workspace gestures are
  fully C; `tests/shell/*` plus a new `tests/shell/overview.js` pass.

### M4 — System UI to C (effort 3.5)
45 modules, 29.3k lines: `quickSettings`, all `status/*` (network, bluetooth,
power, volume, keyboard, …), `dateMenu`/`calendar`, `messageTray`/`messageList`/
`notificationDaemon`, `keyboard` (OSK) and `inputMethod`, `components/*`
(polkit, keyring, network agent, automount), `screenshot`, `magnifier`,
`padOsd`, `lookingGlass`, `endSessionDialog`, `shellMountOperation`, a11y.
* D-Bus heavy: generate all proxies/skeletons with `gdbus-codegen` from
  `data/dbus-interfaces/` instead of hand-writing them.
* Hook points: `quick-settings.opened`, `notification.shown/activated`,
  `screenshot.taken`, `osk.shown/hidden`.
* Exit: Looking Glass is Lua-only; JS remaining is `gdm/`, lock screen, extension
  system and the out-of-process services.

### M5 — Session, auth, services, GJS removal prerequisites (effort 2)
46 modules, 14.5k lines: `screenShield`, `unlockDialog`, all of `js/gdm`,
`extensionSystem`/`extensionDownloader` (rewritten as `ShellExtensionManager`
loading Lua instead of JS), `endSessionDialog`, `welcomeDialog`, the
`dbusServices` processes (extensions prefs, screencast, notifications,
screensaver) as small C daemons sharing `libshell` code, and the WebKit-based
portal helper.
* GDM/lock screen is ported last and gets its own test matrix (password,
  fingerprint, smartcard, FIDO2, SSSD, oVirt, conflicting sessions) using the
  `dbusmock` templates already in `tests/`.
* Exit: `js/` is empty except `init.js`.

### M6 — Delete GJS (effort 0.5)
* [x] Remove `gjs` from `meson.build` (`gjs_dep`, `gjs_req`, the `gjs` program,
  `girepository-2.0`), delete `run-js-test.c`, `js/`, the JS gresource, the
  eslint configuration and CI jobs, the jasmine wrap and `docs/js-coding-style.md`.
* [x] `init.js` responsibilities move into `main.c` (idle start of
  `shell_main_start ()`, the Meta main loop, terminate with error); `main.js`
  is `ShellMain` (`src/ui/main.c`), `components.js` is `src/ui/components.c`,
  `panelItems.js` is `src/ui/panel-items.c`.
* [x] `ShellGlobal` has no `GjsContext`; the Sysprof GJS profiler hooks and
  `ShellMemoryInfo.gjs_*` are gone.
* [x] `g-ir-scanner` for libshell: kept behind the `introspection` Meson option
  (default on, the API reference is built from it); nothing loads the typelibs.
* [x] Replace `gjs_dumpstack` on fatal signals with the Lua traceback of
  the running extension state plus a C backtrace
  (`backtrace_symbols_fd`, or the sanitizer's under ASan).
* [x] Exit: the binary links neither `libgjs` nor `libmozjs` (no reference left
  in the Meson files; CI checks `ldd` of `gnome-shell`, `libshell` and
  `libshell-lua` on every build, see "Check that nothing links GJS or
  SpiderMonkey" in `.github/workflows/ci.yml`).
* [ ] Exit: startup time, RSS and frame time measured against upstream 51.1
  and published in `docs/PERF.md` (the method is written; the numbers need a
  real machine with a GPU, the headless CI runner cannot give them).

### M7 — Extension API v1 (effort 2.5; design starts during M2)
* [x] `docs/api/` is written *before* code: every `shell.*` function with its
  signature, errors, ownership rules, and the hook catalogue with payload schemas.
* [x] Implement the `shell-lua-api-*.c` modules per §3; conformance tests in
  `tests/api/` (one Lua file per API area, run headless: core, ui, menu,
  panel, dash, quick-settings, notify, window, hooks, dbus and the examples).
* [x] Prefs schema + Adw renderer in the extensions D-Bus service.
* [x] Reference extensions in `subprojects/shell-lua/examples/`: top-bar
  indicator with menu, quick-settings toggle with slider, dash button
  launching an app, a hooks-only extension that logs lifecycle events, and a
  prefs example.
* [x] `gnome-extensions` tool: `create` scaffolds Lua; `pack`/`install` unchanged.
* [x] Exit: all examples pass conformance (`tests/api/examples.lua`); API doc
  published; `api_version = 1.0` frozen. The whole suite (11 unit programs, 8
  shell tests, 9 API tests) is green in CI under AddressSanitizer with
  `G_DEBUG=fatal-warnings` since run 90.

### M8 — Stabilization and release (effort 1)
* Two GNOME point releases of soak time on the maintainers' own machines.
* Fuzz the Lua API boundary (malformed handles, re-entrancy from hooks,
  disable-during-callback).
* Write `docs/MIGRATING_FROM_JS.md` (§7) and announce.

## 5. The extension API, in brief

Illustrative only; the normative text lives in `docs/api/` (M7).

```lua
-- metadata.json: {"uuid":"clock@example","shell-api":"1","capabilities":["settings"]}
local shell = require("shell")        -- the only module an extension can load
local ext = {}

function ext.enable()
  local label = shell.ui.label{ text = "…", style_class = "clock-label" }
  local ind = shell.panel.add_indicator{ accessible_name = "Clock", child = label,
                                         position = "right", index = 1 }
  ind.menu:add_item{ label = "Open Clocks", on_activate = function()
    shell.apps.launch("org.gnome.clocks.desktop")
  end }

  local notifications = shell.settings.open("org.gnome.desktop.notifications")
  shell.quick_settings.add_toggle{
    title = "Focus", icon_name = "weather-clear-night-symbolic",
    on_toggle = function(self, checked) notifications:set_boolean("show-banners", not checked) end,
  }

  shell.dash.add_button{ label = "Terminal", icon_name = "utilities-terminal-symbolic",
    on_click = function() shell.apps.launch("org.gnome.Console.desktop") end }

  shell.hooks.on("window.focused", function(ev)
    local w = shell.windows.get(ev["window-id"])
    if w then label.text = w.title end
  end)
  shell.hooks.before("overview.showing", function(ev)
    if ev.reason == "hot-corner" then return shell.hooks.CANCEL end
  end)
  shell.timer.every{ ms = 60000, callback = function() shell.log.info("still running") end }
end

return ext
-- no disable(): everything above is owned by the extension context
```

Design rules for the surface:
* Constructors take a single table; properties are plain fields; events are
  `on_*` callbacks or `h:on_<event>(fn)`; handles carry only the methods
  documented for their type.
* Actors are exposed through `shell.ui.*` only at the level of St widgets and
  layout properties (padding, expand, align, visible, style_class, CSS via the
  extension stylesheet). No Clutter/Cogl, no raw allocation/paint.
* Hooks: `shell.hooks.before` can cancel (`shell.hooks.CANCEL`) or amend (return
  a table) on cancellable hooks, `shell.hooks.on` is informational (after).
* Everything else (D-Bus, files, processes, network) is behind capabilities and
  implemented in C with GVariant↔Lua mapping limited to basic types, arrays,
  dictionaries and tuples.

## 6. Risks

| Risk | Mitigation |
|---|---|
| Sheer size of the JS→C port (≈85k JS → ~120–150k C). | Leaf-first order keeps the shell usable at every step; file-per-module mapping lets several people work in parallel; `gdbus-codegen` and the existing St/Shell classes absorb a large fraction. Track in `PORT_STATUS.md`. |
| Upstream gnome-shell keeps moving (two releases per year) while the port is in flight. | Pin to 51.x for M0–M6; merge upstream only into already-ported C modules and unported JS modules, never half-way; GI annotations on new C keep upstream JS patches applicable. |
| Mutter API bump every cycle. | Same pinning; a release of gnome-lua tracks exactly one mutter API version. |
| Lua 5.5 is young; vendoring it means owning CVE tracking. | Wrap file pinned to a tag with checksum; update with each point release; `-DLUA_USE_LINUX`, no `os`/`io` in extension states shrinks the exposed surface. |
| Lifetime bugs at the C↔Lua boundary (dangling handles, re-entrancy when a hook destroys what emitted it). | Handles invalidated on `dispose`; hook emission snapshots listener list; API calls that mutate the shell are deferred to idle when emitted from inside a hook; fuzzing in M8. |
| An API that is too narrow drives extension authors away. | Ship the four requested areas plus hooks in 1.0; add surface by minor versions based on a public request tracker; `capabilities` give an escape hatch (D-Bus) without exposing internals. |
| GDM/lock screen regressions are security-relevant. | Port last, dedicated `dbusmock` test matrix, manual sign-off checklist before M6. |
| Performance regressions vs. GJS JIT in the remaining dynamic parts. | Nothing dynamic remains on the shell's hot path; extension callbacks are bounded by the watchdog; measure in M6. |

## 7. Migration notes for JS extension authors (to be expanded in M7)

Full guide: [`MIGRATING_FROM_JS.md`](MIGRATING_FROM_JS.md). In brief:

* `Main.panel.addToStatusArea(...)` → `shell.panel.add_indicator{...}`.
* `QuickSettings.QuickToggle` subclasses → `shell.quick_settings.add_toggle{...}`.
* `Main.overview.dash` manipulation → `shell.dash.add_button{...}`.
* `InjectionManager` / prototype patching → `shell.hooks.on("…")`; if the hook you
  need does not exist, file a request; patching internals is not possible by design.
* `GObject.registerClass` widgets → compose `shell.ui.*` widgets; custom drawing is
  limited to icons/images and CSS.
* GTK prefs windows → declarative `prefs.lua` schema.

## 8. Repository layout (after M0)

```
docs/PLAN.md, PORT_STATUS.md, PERF.md, api/, MIGRATING_FROM_JS.md
src/            C core (unchanged layout) + src/ui/, src/gdm/, src/misc/, src/lua-api/ (M7)
subprojects/    shell-lua/ (runtime, GLib-only, standalone-testable), lua.wrap, existing gvc, shew, …
js/             shrinks to zero by M6
examples/       reference Lua extensions
tests/shell, tests/unit (C), subprojects/shell-lua/tests, tests/api
```

## 9. Immediate next steps

1. Confirm the interpretation at the top of this document.
2. M0: import gnome-shell 51.1 history into this repo and get CI green.
3. M1: land `subprojects/shell-lua/` with the sandboxed state, watchdog and `examples/hello@gnome-lua.example`.
4. Start `docs/api/` with the hook catalogue, since M2–M5 must emit the hooks.
