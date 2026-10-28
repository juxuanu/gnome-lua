# C conventions for the JS to C port

Applies to every new file under `src/ui/`, `src/misc/` and `src/gdm/`. It adds to
`docs/c-coding-style.md` (GNU/GNOME style, 2-space indent, space before
parentheses, `g_autoptr (T) x = NULL;`); where the two disagree, that file wins.
The reference style is `src/shell-app-system.c`, `src/shell-window-tracker.c` and
`src/st/st-button.c`.

## Files

* `js/ui/panelMenu.js` becomes `src/ui/panel-menu.c` + `src/ui/panel-menu.h`:
  same directory as the JS (`ui`, `misc`, `gdm`), camelCase turned into
  kebab-case. Subdirectories keep their names (`js/ui/status/network.js` ->
  `src/ui/status/network.c`). One header per module, no `-private.h` unless
  something is shared only between C files of the same module.
* First line `/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */`,
  then `#include "config.h"` in `.c`, `#pragma once` in `.h`.
* Type prefix is `Shell` (`ShellPanelMenuButton`, `shell_panel_menu_button_*`).
  Widgets that are pure toolkit stay in `st/` with the `St` prefix.
* Add the file to `src/meson.build` (GIR source list while JS remains) and
  `po/POTFILES.in` if it has translatable strings.

## Types

* Leaf class: `G_DECLARE_FINAL_TYPE` in the header, struct in the `.c`:

  ```c
  #define SHELL_TYPE_PANEL_BUTTON (shell_panel_button_get_type ())
  G_DECLARE_FINAL_TYPE (ShellPanelButton, shell_panel_button,
                        SHELL, PANEL_BUTTON, StButton)
  ```

  ```c
  struct _ShellPanelButton { StButton parent; ... };
  G_DEFINE_FINAL_TYPE (ShellPanelButton, shell_panel_button, ST_TYPE_BUTTON)
  ```

* Use `G_DECLARE_DERIVABLE_TYPE` only when another module (C or still-JS via
  `GObject.registerClass`) subclasses it, e.g. `PopupBaseMenuItem`. Put the class
  struct in the header, keep instance data in a private struct
  (`G_DEFINE_TYPE_WITH_PRIVATE`), as `StButton` does. Every JS virtual method
  override that exists upstream (`allocate`, `get_preferred_width`,
  `style_changed`, `key_press_event`, ...) becomes a vfunc override in
  `class_init`, never a signal handler on self.
* A JS class using `Signals.EventEmitter` and no actor becomes a plain `GObject`.
* Instance variable names are descriptive (`menu`, `button`), as in
  `docs/c-coding-style.md`; `self` is acceptable only in code that already uses it.
* Use `uint32_t`, `int64_t`, etc. for new code; `gboolean`, `guint` for signal
  and source IDs only.

## Memory

* `g_autoptr (T)`, `g_autofree`, `g_autolist`, `g_clear_object`,
  `g_clear_pointer` for everything; `g_steal_pointer` when handing ownership out.
* Every `dispose` clears sources (`g_clear_handle_id (&id, g_source_remove)`),
  disconnects handlers it made on foreign objects, and drops child actors.
  `finalize` frees plain memory only. This replaces JS `connectObject (..., this)`
  and `_onDestroy`; use `g_signal_connect_object (obj, ..., self, 0)` so the
  handler dies with `self`.
* Actors: `clutter_actor_destroy ()` in `dispose`, never `g_object_unref ()`.

## Properties and signals

Copy the `ShellWindowTracker` shape: an anonymous enum with `PROP_0 ... N_PROPS`,
a `static GParamSpec *props[N_PROPS]`, an enum plus `static guint signals[]`.

```c
enum { PROP_0, PROP_ACTIVE, N_PROPS };
static GParamSpec *props[N_PROPS] = { NULL, };
enum { ACTIVATED, LAST_SIGNAL };
static guint signals[LAST_SIGNAL] = { 0 };
```

* Property names are the JS names in kebab-case (`_activeApp` -> `active-app`).
  `NULL, NULL` for nick/blurb; flags always include `G_PARAM_STATIC_STRINGS`;
  add `G_PARAM_EXPLICIT_NOTIFY` and call `g_object_notify_by_pspec (obj,
  props[PROP_X])` only when the value changes.
* Signals: same kebab-case names as the JS `this.emit ('name')`, created in
  `class_init` with `g_signal_new` (give a vfunc offset if C subclasses need it).
  Use a marshaller only when the signature is not covered by
  `g_cclosure_marshal_generic`.
* `get_property`/`set_property` always have a `default:` that calls
  `G_OBJECT_WARN_INVALID_PROPERTY_ID`.

## GI annotations (mandatory while `js/` exists)

The remaining JS imports the ported module through GI, so the `.c` must be
annotated like the rest of `src/`: every public function gets gtk-doc comments
with `(transfer ...)`, `(nullable)`, `(out)`, `(element-type ...)`, `(closure)`,
`(scope async)` as needed, and `Returns:`. Check
the generated `Shell-*.gir` under `build/src/` after adding a function; a missing annotation is a bug
in the port, not in the JS caller. Names JS used must stay reachable: keep
property/signal names, and add `shell_<module>_<method>` for each public JS
method. The annotations become optional only after M6 removes `g-ir-scanner`.

## Former `Main.*` singletons

No new `static Foo *default_instance` outside what already exists. The JS
globals (`Main.panel`, `Main.overview`, `Main.layoutManager`, `Main.messageTray`,
`Main.sessionMode`, ...) become accessors on `ShellGlobal` next to
`shell_global_get_window_tracker ()` and `shell_global_get_app_system ()`:

```c
ShellPanel *shell_global_get_panel (ShellGlobal *global); /* (transfer none) */
```

The owner (`ShellMain` after `main.js` is ported) creates the object and stores
it in `ShellGlobal`; callers fetch it through `shell_global_get ()`. Modules
never hold a cached pointer across a session-mode change. Existing
`*_get_default ()` functions stay but new code prefers the `ShellGlobal`
accessor.

## Async

JS `async`/`await` and `_promisify` map to the GIO pattern, one pair per
operation, exactly like `shell_util_touch_file_async/_finish`:

```c
void     shell_foo_do_async  (ShellFoo *foo, GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data);
gboolean shell_foo_do_finish (ShellFoo *foo, GAsyncResult *res, GError **error);
```

Create with `g_task_new (foo, cancellable, callback, user_data)`, call
`g_task_set_source_tag`, return through `g_task_return_*`, and
`g_task_propagate_*` in `_finish`. Callbacks that outlive `self` use
`g_task_get_source_object`/weak refs. No nested main loops. JS callers get
promises back through `Gio._promisify` on these pairs.

## D-Bus

Never hand-write proxies or skeletons. Put the XML in `data/dbus-interfaces/`
(copy of the interface, as for `net.hadess.SwitcherooControl.xml`) and add
`gnome.gdbus_codegen (...)` with `namespace: 'Shell'` in `src/meson.build`. Use
the generated `ShellFoo` proxy (`shell_foo_proxy_new_for_bus`) and skeleton
(`shell_foo_skeleton_new`, `g_dbus_interface_skeleton_export`). Exported names and
object paths must not change (PLAN D11).

## Hooks

Emit through `ShellHookRegistry` (`subprojects/shell-lua/src/shell-hooks.h`),
never call extension code directly.

* Name: `<subject>.<event>`, lower-case, one dot, hyphens inside a part
  (`panel.indicator-added`, `overview.showing`, `window.focused`). Subject is the
  noun owning the state, event is a present participle (`showing`, `hiding`,
  cancellable, "before") or a past form (`shown`, `hidden`, informational).
* Declare once at module init with `shell_hook_registry_declare (reg, name,
  "key:type ...", cancellable, doc)`. `cancellable` is true only if the code
  can really abort or amend the action. Every hook added needs a line in
  `docs/api/` in the same commit.
* Payload is an `a{sv}` GVariant of basic types: keys lower-case kebab-case;
  refer to objects by id (`app-id:s`, `window-id:t`), never by pointer.
* Order: `emit_before ()` (if cancellable), do the action unless
  `SHELL_HOOK_VERDICT_CANCEL`, then `emit_after ()`. Emit from the state transition,
  not from a signal handler that can fire twice. Hooks must not be emitted from
  `dispose`.

## Errors

* Programmer errors: `g_return_if_fail`/`g_return_val_if_fail` on public entry
  points; `g_assert_not_reached ()` for impossible switch cases.
* Runtime failures: `GError **` with `G_IO_ERROR`/domain codes, or `g_task_return_error`.
  JS `catch (e) { logError (e) }` becomes `g_warning ("%s: %s", G_STRFUNC, error->message)`
  and execution continues; only ignore `G_IO_ERROR_CANCELLED` silently.
* Tests run with `G_DEBUG=fatal-warnings`: no warning may fire in normal paths.

## Port workflow

1. Port one module (plus its tests); delete its `js/` file and its entry in
   `js/js-resources.gresource.xml`, fix remaining JS imports.
2. Update `docs/PORT_STATUS.md` in the **same commit**: set status `ported`
   (or `wip` if split over several commits) and note any behaviour change.
3. Build with `--werror`, run `meson test`; `tests/shell/*` must still pass.
4. Commit subject is exactly `ui: port <module> to C`, using the JS base name
   (`ui: port panelMenu to C`, `misc: port util to C`, `gdm: port loginDialog to C`).
   The body lists the hooks added and the JS API that changed. No unrelated edits.

## Pitfalls found by CI

* `ClutterActor` creates the class layout manager in `constructed()`. In a
  subclass of `StBoxLayout` (or any actor with a class layout manager) never
  call `st_box_layout_set_orientation()` or touch the layout manager from
  `init()`; override `constructed()`, chain up, then set it. Non-construct
  properties passed to `g_object_new()` are applied after `constructed()`.
* GI flags values must be bit combinations of the declared members; `~0`
  is rejected by GJS for a flags argument.
* `(closure user_data) (destroy destroy)` go on the callback parameter's
  annotation, never on `user_data`; `g-ir-scanner` runs with fatal warnings.
* Asynchronous completions must not fire into JS during shutdown; ease
  completions are dropped by `_shell_ease_shutdown()`, your own timeouts and
  GTasks must be cleared on `ShellGlobal::shutdown` or made no-ops.
* A public method named `shell_<type>_init`, `_dispose`, `_finalize` or `_constructed` collides with the
  GObject instance functions; the JS `init()` of `LayoutManager` became `begin_startup()`.
* GI methods have no optional arguments: JS callers of a ported `addChrome (actor, params)` must now pass
  every argument (`add_chrome (actor, Shell.ChromeFlags.NONE)`). Prefer a flags argument over several
  booleans, and a second function (`track_chrome_with_flags ()`) over an "unset" value.
* Boxed values (`ShellMonitor`) returned from properties or getters are copies for JS: object identity
  (`===`) between two reads is gone, compare an index.
