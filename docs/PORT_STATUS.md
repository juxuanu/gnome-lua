# Port status

One row per upstream GJS module (gnome-shell 51.1, `js/`). Each module is ported to C under `src/ui/` (or `src/gdm/`, `src/misc/`) keeping the same base name, so upstream diffs map one to one. The JS file is deleted in the same commit that lands its C replacement.

Status values: `todo`, `wip`, `ported`, `dropped` (with a reason). Update this file in the same commit as the port. See `docs/PLAN.md` for what the milestones mean.

| Milestone | files | JS lines |
|---|---:|---:|
| M2 | 57 | 20795 |
| M3 | 26 | 19967 |
| M4 | 45 | 29338 |
| M5 | 46 | 14475 |
| total | 174 | 84575 |

## M2

| Upstream module | JS lines | Status | Notes |
|---|---:|---|---|
| `js/ui/popupMenu.js` | 1683 | ported | `src/ui/popup-menu.[ch]`: `Shell.PopupBaseMenuItem` (derivable St.BoxLayout), `PopupMenuItem`, `PopupSeparatorMenuItem`, `PopupSwitchMenuItem`, `PopupImageMenuItem`, `PopupSubMenuMenuItem`, `Shell.Switch` (final), `Shell.PopupMenuBase` (abstract GObject, `actor` property), `PopupMenu`, `PopupSubMenu`, `PopupMenuSection` (derivable), `PopupDummyMenu`, `PopupMenuManager` (final). `Shell.Ornament` replaces `PopupMenu.Ornament`; `arrowIcon()` is `Shell.popup_menu_arrow_icon()`. Constructors take construct properties: `new Shell.PopupMenu({source_actor, arrow_alignment, arrow_side})`, `new Shell.PopupMenuItem({text})`, `new Shell.PopupSeparatorMenuItem({text})`, `new Shell.PopupSwitchMenuItem({text, state})`, `new Shell.PopupImageMenuItem({text, gicon \| icon_name})`, `new Shell.PopupSubMenuMenuItem({text, want_icon})`, `new Shell.Switch({state})`, `new Shell.PopupDummyMenu({source_actor})`, `new Shell.PopupMenuManager({action_mode})` (the `owner` argument was unused), `Shell.PopupMenuBase` subclasses pass `{source_actor, style_class}`. JS subclasses of menus and items are registered classes (`static { GObject.registerClass(this); }` and `_init`, no `constructor`). The item params `{reactive, activate, hover, style_class}` are `activatable`, `active_on_hover` and `extra_style_class` construct properties; since `reactive` is applied after construction, a non reactive item sets `reactive: false, activatable: false, active_on_hover: false`. Menu methods: `addMenuItem(i)` is `add_menu_item(i)`, `addMenuItem(i, pos)` is `insert_menu_item(i, pos)`, `moveMenuItem`, `removeAll`, `addSettingsAction`, `isEmpty`, `setArrowOrigin`, `setSourceAlignment`, `_getMenuItems` (`get_menu_items`), `addAction(t, cb)` is `add_action(t, cb)` and `addAction(t, cb, icon)` is `add_action_with_icon(t, gicon, icon_name, cb)`; `isOpen` is `is_open`, `sourceActor` is `source_actor`, `focusActor` is `focus_actor`, `_boxPointer` is `box_pointer`, `setSensitive(b)`/`sensitive` are `set_sensitive(b)`/`sensitive`; `open(params)`/`close(params)` take a `Shell.PopupAnimation` (`{animate: false}` is NONE, `{fadeOnly: true}` is FADE, no argument is FULL, which must now be passed explicitly; `triggerEvent` is gone). Items: `setOrnament` is `set_ornament`, `setStatus`, `setToggleState`, `setSubmenuShown`, `setIcon(x)` is the `gicon`/`icon_name` properties, `_ornamentIcon` is `ornament_icon`, `activate(event)` always takes an event (`null` is fine). Overrides become vfuncs: `open`/`close`/`destroy`/`isEmpty`/`getSensitive` of menus are `vfunc_open(animate)`/`vfunc_close(animate)`/`vfunc_destroy()`/`vfunc_is_empty()`/`vfunc_get_sensitive()`, and `activate`/`syncSensitive`/`_setParent`/`_updateOrnamentStyle` of items are `vfunc_activate(event)`/`vfunc_sync_sensitive()`/`vfunc_set_parent_menu(parent)`/`vfunc_update_ornament_style()`. `actor._delegate` is `Shell.PopupMenuBase.get_from_actor(actor)`; `menuItem.actor` is still the item. `PopupMenuManager.activeMenu` is `active_menu`, `addMenu(m)` is `add_menu(m)` (`insert_menu(m, pos)` for a position), and it accepts `PopupDummyMenu` too. `Main.sessionMode`, `layoutManager` `system-modal-opened` and `Main.overview.hide()` + `Main.panel.closeQuickSettings()` (settings items) are reached through `Shell.Main` (`session-mode` property, `system-modal-opened()` and the `settings-app-activating` signal), wired in main.js. Not preserved: the `length` of a menu also drops when a plain item is destroyed; the `ignoreRelease()`/`blockSourceEvents` APIs do not exist in 51.1 and were not ported. |
| `js/misc/breakManager.js` | 1480 | todo | |
| `js/misc/timeLimitsManager.js` | 1475 | todo | |
| `js/ui/layout.js` | 1430 | todo | |
| `js/ui/main.js` | 1069 | wip | modal stack (pushModal/popModal) in C as ShellMain (`src/ui/main.[ch]`, `Shell.Main.get_default()`). `Main.pushModal()`/`popModal()` are thin wrappers; `Main.modalCount`/`Main.actionMode` mirror the `modal-count`/`action-mode` properties; `modal-ended` is connected to `layoutManager.modalEnded()`. `Main.modalActorFocusStack` is gone. `popModal()` still throws "incorrect pop" (a GError from C). |
| `js/ui/background.js` | 997 | ported | `src/ui/background.[ch]`: `ShellBackgroundManager` (final GObject) and `ShellSystemBackground` (a `ClutterActor` wrapping the `Meta.BackgroundActor`, because that type is final in C); the cache, source, `Background`, glycin texture cache and slide-show `Animation` are private to the C file. `new Background.BackgroundManager({container, monitorIndex, layoutManager, controlPosition, useContentSize, vignette, settingsSchema})` is `new Shell.BackgroundManager({container, monitor_index, control_position, use_content_size, vignette, settings_schema})`: `layoutManager` is gone (monitor geometry comes from `MetaDisplay`), `.backgroundActor` is `.background_actor`, `destroy()` and the `changed`/`loaded` signals are unchanged. `new Background.SystemBackground()` is `new Shell.SystemBackground()`. New `Shell.BackgroundManager.set_screen_transition(actor)`, called by layout.js with `Main.layoutManager.screenTransition` (the C code cannot reach that JS-only actor). Textures load through the async glycin API (JS used the blocking calls). The no-key-frame animation path re-applies the color/gradient (JS called the removed `Meta.Background.set_file()`, which threw). Needs `gnome-bg-4` besides `gnome-desktop-4`. |
| `js/ui/panel.js` | 786 | todo | |
| `js/ui/boxpointer.js` | 689 | ported | `src/ui/box-pointer.[ch]` (`ShellBoxPointer`, derivable; `ShellPopupAnimation` flags). `new BoxPointer(side)` is now `new Shell.BoxPointer({arrow_side: side})`; methods are snake_case (`set_position`, `set_arrow_origin`, `open(animate, onComplete)`, ...), `.arrowSide` is `.arrow_side`, `BoxPointer.PopupAnimation` is `Shell.PopupAnimation`. `bin` is a read-only property. New `arrow-alignment`/`arrow-actor` properties. Monitor/work area come straight from MetaDisplay (workspace 0), falling back to the primary monitor for an off-screen source. |
| `js/ui/dnd.js` | 643 | todo | |
| `js/ui/shellDBus.js` | 634 | todo | |
| `js/ui/workspaceAnimation.js` | 599 | todo | |
| `js/misc/brightnessManager.js` | 551 | todo | |
| `js/misc/systemActions.js` | 525 | ported | `src/misc/system-actions.[ch]`: `ShellSystemActions` singleton, `Shell.SystemActions.get_default()` (was `new SystemActions.getDefault()`). The `can-*` properties and `orientation-lock-icon` keep their names (read-only, notified only when the value changes; JS notified on every update); new `can_*` getters. Methods are snake_case: `get_matching_actions(terms)`, `get_name(id)`, `get_icon_name(id)`, `activate_action(id)`, `activate_{power_off,restart,suspend,lock_screen,logout,switch_user,lock_orientation,screenshot_ui}()`, `force_update()`; an unavailable action throws a `GLib.Error` (G_IO_ERROR_NOT_SUPPORTED, same message as the JS `Error`). `Main.sessionMode` is the new `session-mode` property, set once by main.js (until then the session counts as neither locked nor greeter; the actions are updated when it is set and on its `updated`). The JS-only objects are reached through signals that main.js connects: `lock-screen-requested(animate)` -> `screenShield?.lock(animate)` (lock screen, switch user), `hide-overview-requested` -> `overview.hide()` (logout), `screenshot-ui-requested` -> wait for `overview` `hidden`, then `Screenshot.showScreenshotUI()`. `Main.layoutManager` `monitors-changed` is the `MetaMonitorManager` signal of the same name. Missing GSettings schemas (e.g. `org.gnome.login-screen` without GDM) log a warning and read as `false` instead of throwing. Adds the `accountsservice` and `gdm` (libgdm) build dependencies (`ActUserManager`, `gdm_get_session_ids()`, `gdm_goto_login_session_sync()`). Callers in appDisplay, status/system, status/autoRotate, breakManager and timeLimitsManager use `Shell.SystemActions`. |
| `js/misc/inputMethod.js` | 480 | ported | `src/misc/input-method.[ch]`: `ShellInputMethod`, a final `Clutter.InputMethod` subclass (`new InputMethod.InputMethod()` is `new Shell.InputMethod()`; the `vfunc_*` overrides are class vfuncs). It no longer reaches into JS: `Main.inputMethod.has_input_source` is set by the input source manager (`_currentInputSourceChanged`) and `osk_visible` by the OSK manager (`keyboard.js`, also cleared when the OSK is destroyed), replacing the `current-source-changed` / `Main.keyboard` `visibility-changed` listeners. `currentFocus` is `current_focus` (read-only property), `getSurroundingText()` is `get_surrounding_text()`, `hasPreedit()` is `has_preedit()`, `handleVirtualKey(keyval)` is `handle_virtual_key_async(keyval, null)` (promisify in keyboard.js; resolves to a boolean, never rejects). `update()` and the `surrounding-text-set` signal are unchanged. The `try/catch` fallbacks for mutter without `set_preedit_text_with_attrs` / negative `delete_surrounding` offsets are gone; IBus >= 1.5.33 is required. A second `focus_out` without a `focus_in` now replaces the pending panel-hide timeout instead of leaving two. `get_surrounding_text()` returns `[null, 0, 0]` (JS: `[null, null, null]`) when the focus has no surrounding text. On `ShellGlobal::shutdown` the panel-hide timeout and the IBus context are dropped and pending virtual-key promises never settle. |
| `js/misc/util.js` | 471 | todo | |
| `js/ui/environment.js` | 464 | wip | ease helpers in C; remaining JS is the GJS bootstrap, deleted in M6 |
| `js/misc/ibusManager.js` | 372 | ported | `src/misc/ibus-manager.[ch]`: `ShellIBusManager` singleton, `Shell.IBusManager.get_default()` (was `getIBusManager()`). Methods are snake_case: `restart_daemon(args|null)` and `set_engine(id)` are fire and forget, `get_engine_desc(id)`, `preload_engines(ids)` (copies `ids`), `activate_property(key, state)`, `set_completion_enabled_async(enabled, null)` / `_finish` (resolves to a boolean; promisify in keyboard.js). Signals `ready`, `properties-registered`, `property-updated`, `set-content-type`, `focus-in`, `focus-out` are unchanged; `set-cursor-location` passes `(x, y, width, height)` instead of a `{x, y, width, height}` object. The candidate popup is no longer created by the manager (it is still JS): `InputSourceManager` creates it and follows the new read-only `panel-service` property (`notify::panel-service` -> `setPanelService()`) and the new `engine-changed (name)` signal (-> `close(Shell.PopupAnimation.NONE, null)`; the JS passed `{animate: false}`, which `Shell.BoxPointer.close()` no longer accepts). Needs `ibus-1.0 >= 1.5.33` (new meson dependency); the runtime `_checkIBusVersion` checks became build-time checks. On `ShellGlobal::shutdown` the manager cancels its pending calls and the preload timeout and disconnects from IBus, so no completion reaches JS afterwards. |
| `js/ui/dialog.js` | 352 | ported | `src/ui/dialog.[ch]`: `ShellDialog` and `ShellMessageDialogContent` (derivable), final `ShellListSection`/`ShellListSectionItem`. `new Dialog.Dialog(parent, cls)` is `new Shell.Dialog({parent_actor, dialog_style_class})`; `addButton({label, action, key, default})` is `add_button(label, key || 0, !!default, action)` (returns the St.Button, no `buttonInfo.button` back-reference); `contentLayout`/`buttonLayout`/`initialKeyFocus`/`clearButtons`/`makeInactive` are `content_layout`/`button_layout`/`initial_key_focus`/`clear_buttons`/`make_inactive`; the private `_dialog` frame is the read-only `frame` property. `ModalDialog.addButton(buttonInfo)` is unchanged. |
| `js/misc/weather.js` | 327 | todo | |
| `js/misc/keyboardManager.js` | 309 | ported | `src/misc/keyboard-manager.[ch]`: `ShellKeyboardManager` singleton, `Shell.KeyboardManager.get_default()` (was `getKeyboardManager()`), which also owns the `GnomeDesktop.XkbInfo` (`get_xkb_info()`, was `getXkbInfo()`). `setUserLayouts`, `setKeyboardOptions`, `setKeyboardModel`, `isLocked`, `isExternal` are snake_case; `apply(id)` and `reapply()` are fire and forget (a failed `set_keymap_async` is logged as a warning). `shortName`/`displayName` are the read-only properties `short_name`/`display_name`; `currentLayout` is only `get_current_layout_id()`. `DEFAULT_LAYOUT`, `DEFAULT_LOCALE`, `DEFAULT_VARIANT` are `Shell.KEYBOARD_MANAGER_DEFAULT_LAYOUT` etc., `MAX_LAYOUTS_PER_GROUP` is `Shell.KEYBOARD_MANAGER_MAX_LAYOUTS_PER_GROUP`. `keymap-changed` is unchanged. |
| `js/misc/jsParse.js` | 302 | dropped | JS tokenizer for the Looking Glass JS evaluator; the Lua REPL replaces it. Removed in M6. |
| `js/misc/modemManager.js` | 302 | todo | |
| `js/misc/signalTracker.js` | 287 | dropped | GJS connectObject tracking; C uses g_signal_connect_object. Removed in M6. |
| `js/misc/loginManager.js` | 279 | ported | `src/misc/login-manager.[ch]`: `ShellLoginManager` singleton; LoginManagerSystemd/Dummy are one class (no logind proxy = dummy). Session/User/Manager proxies are gdbus-codegen types (`Shell.Login1Session`, `Shell.Login1User`): properties are lower-case GObject properties, `connectSignal('Lock')` is `connect('lock')`. `_finish` errors propagate instead of being logged and resolved as null. `canRebootToBootLoaderMenu()` is `can_reboot_to_boot_loader_menu_async()` resolving to `[can, needsAuth]`; `listSessions()` resolves to a `GLib.Variant` (`deepUnpack()`). |
| `js/ui/barLevel.js` | 271 | ported | `src/ui/bar-level.[ch]` (`ShellBarLevel`, derivable). `overdrive-start` above the maximum now logs a warning instead of throwing. |
| `js/misc/parentalControlsManager.js` | 267 | todo | |
| `js/ui/modalDialog.js` | 266 | ported | `src/ui/modal-dialog.[ch]`: `ShellModalDialog` (derivable `St.Widget`), `Shell.ModalDialogState` (`OPENED`, `CLOSED`, `OPENING`, `CLOSING`, `FADED_OUT`, same values as `ModalDialog.State`). `extends ModalDialog.ModalDialog` is `extends Shell.ModalDialog`; `ModalDialog.State.X` is `Shell.ModalDialogState.X`; `state` is a read-only property with notify. The `_init(params)` object became construct properties: `styleClass` is `dialog_style_class` (the style class of the inner `Shell.Dialog` frame, not the widget's `style_class`), `shellReactive` `shell_reactive`, `actionMode` `action_mode` (`Shell.ActionMode` flags, default `SYSTEM_MODAL`), `shouldFadeIn`/`shouldFadeOut`/`destroyOnClose` `should_fade_in`/`should_fade_out`/`destroy_on_close`, plus a new construct-only `container`. `contentLayout`/`buttonLayout`/`dialogLayout`/`backgroundStack` are the read-only properties `content_layout`/`button_layout`/`dialog_layout`/`background_stack`. `addButton({label, action, key, default})` is `add_button(label, key \|\| 0, !!default, action)` and returns the `St.Button` (use it instead of the removed `buttonInfo.button`), `clearButtons()` is `clear_buttons()`, `setButtons([...])` is `clear_buttons()` followed by `add_button()` for each (shellMountOperation keeps a local helper), `setInitialKeyFocus(a)` is `set_initial_key_focus(a)`, `popModal()`/`pushModal()` are `pop_modal()`/`push_modal()`, `_fadeOutDialog()` is the public `fade_out_dialog()`. `open()` and `close()` take no arguments and are virtual functions: subclasses override `vfunc_open()` (returns a boolean) and `vfunc_close()` and chain up with `super.vfunc_open()`/`super.vfunc_close()`; callers still call `dialog.open()`/`dialog.close()`. The `opened` and `closed` signals are unchanged. `Main.layoutManager.modalDialogGroup` is reached through `Shell.ModalDialog.set_default_container(actor)`, which main.js calls right after creating the layout manager (a dialog may also be constructed with `container`). `Layout.MonitorConstraint` is replaced by a private constraint reading `meta_display_get_monitor_geometry()` of the monitor under the pointer; `Main.layoutManager.emit('system-modal-opened')` is `Shell.Main.system_modal_opened()`, and main.js forwards that signal to the layout manager so its JS listeners (quick settings, switcher popup) still run. Not preserved: `popModal()` failing with "incorrect pop" logs a warning instead of throwing; `endSessionDialog`'s `close(skipSignal)` is `_closeSilently()`; `polkitAgent`'s ignored `reactive: false` button option is gone. |
| `js/ui/grabHelper.js` | 242 | ported | `src/ui/grab-helper.[ch]` (`ShellGrabHelper`, final). `new Shell.GrabHelper({owner, action_mode})`; `grab(actor, focus, on_ungrab)` (`focus`/callback nullable), `grab_async(actor, focus)` + `grab_finish` (promisify in screenshot.js), `ungrab(is_user)`, `ungrab_actor(actor, is_user)`, `is_actor_grabbed()`, `grabbed` property, `get_current_actor()`. `grabStack` is not exposed. Pushes the modal through `shell_main_push_modal()`. |
| `js/misc/objectManager.js` | 241 | todo | |
| `js/misc/dateUtils.js` | 230 | todo | |
| `js/ui/slider.js` | 230 | ported | `src/ui/slider.[ch]` (`ShellSlider`, final). `new Slider(v)` is now `new Shell.Slider({value: v})`. |
| `js/misc/introspect.js` | 218 | todo | |
| `js/ui/sessionMode.js` | 209 | ported | `src/ui/session-mode.[ch]` (`ShellSessionMode`, final GObject). `new SessionMode.SessionMode()` is now `new Shell.SessionMode()`; the mode fields are read-only properties in snake_case (`is_locked`, `has_overview`, `allow_settings`, ...), `currentMode`/`parentMode` are `current_mode`/`parent_mode`, `panel.left/center/right` are the GStrv properties `panel_left/panel_center/panel_right`, `pushMode`/`popMode`/`switchMode` are `push_mode`/`pop_mode`/`switch_mode` (they throw a GError for an unknown mode or an invalid pop). `unlockDialog` is a string (`login-dialog`/`unlock-dialog`, null if the mode cannot lock) mapped to the JS class by `Main.getUnlockDialogClass()`. `get_field(name)` returns the raw JSON-derived GVariant of any other key. `session.mode-changed` is emitted from C after `updated` (the JS emission is gone). `listModes.js` calls `Shell.SessionMode.list_modes()`. |
| `js/ui/osdWindow.js` | 199 | ported | `src/ui/osd-window.[ch]` (`ShellOsdWindow`, final St.Widget per monitor; `ShellOsdWindowManager`, final GObject). `new OsdWindowManager()` is now `new Shell.OsdWindowManager({ui_group: uiGroup})`. `showOne`/`showAll`/`hideAll` are `show_one`/`show_all`/`hide_all`; "no level" is a NaN `level` (was undefined/null), a max level below 1 means 1; `show(icon, label, levels)` takes a `GLib.Variant('a(idd)')` of `[monitor, level, maxLevel]` instead of an object. Windows go straight into `uiGroup` (no addChrome), so no JS shim; the `Layout.MonitorConstraint` is an `allocate` override and the box is centered by a `BinLayout`. |
| `js/ui/panelMenu.js` | 199 | todo | |
| `js/misc/extensionUtils.js` | 172 | dropped | JS extension loading; replaced by ShellLuaExtensionManager (M5). Removed in M6. |
| `js/ui/pointerA11yTimeout.js` | 136 | ported | `src/ui/pointer-a11y-timeout.[ch]` (`ShellPointerA11yTimeout`, final; the pie timer actor is private to the C file). main.js now does `Main.pointerA11yTimeout = new Shell.PointerA11yTimeout({ui_group: uiGroup})` and must keep the reference (the JS class lived on through its seat signal closures). |
| `js/ui/xdndHandler.js` | 127 | ported | `src/ui/xdnd-handler.[ch]` (`ShellXdndHandler`, final). Signals `drag-begin (time)`, `drag-end`, new `drag-motion (x, y, drag-actor, target-actor)`; the drag-monitor / `_delegate.handleDragOver` dispatch is now `DND.handleXdndMotion()` in dnd.js, wired in main.js. `Main.sessionMode.isLocked` is mirrored into the `session-locked` property by main.js. |
| `js/misc/animationUtils.js` | 124 | wip | adjustAnimationTime in C |
| `js/ui/osdMonitorLabeler.js` | 119 | ported | `src/ui/osd-monitor-labeler.[ch]` (`ShellOsdMonitorLabeler`, final; the per-monitor label actor is private to the C file). `new Shell.OsdMonitorLabeler({ui_group: uiGroup})`; `show(client, params)` takes the `a{sv}` as a `GLib.Variant` (shellDBus.js rebuilds it from the unpacked dict). |
| `js/ui/ripples.js` | 112 | ported | `src/ui/ripples.[ch]` (`ShellRipples`, final GObject). `new Ripples(px, py, cls, req)` is now `Shell.Ripples.new(px, py, cls, req)`; `addTo`/`playAnimation` are `add_to`/`play_animation`. Calling `add_to` twice or `play_animation` before `add_to` logs a critical instead of throwing. |
| `js/misc/history.js` | 111 | ported | `src/misc/history.[ch]`: `ShellHistoryManager` (final GObject). `new History.HistoryManager({gsettingsKey, limit, entry})` is `new Shell.HistoryManager({gsettings_key, limit, entry})` (construct-only; `entry` is a `Clutter.Text`, i.e. `entry.clutter_text`, as both callers already passed); `addItem()` / `lastItem()` are `add_item()` / `last_item()`; the `changed` signal is unchanged. The Up/Down `Clutter.KeyController` is installed by C. `trim()` is reimplemented (white space, line terminators, BOM) and the JS quirk of stashing the text being edited into the history array while navigating is kept. Callers: runDialog, lookingGlass. |
| `js/misc/errorUtils.js` | 91 | dropped | JS error formatting for GJS exceptions. Removed in M6. |
| `js/ui/windowAttentionHandler.js` | 91 | todo | blocked on messageTray (M4): ~85% of the file is the `MessageTray.Source`/`Notification` glue, the C-able signal filter is about ten lines |
| `js/misc/dbusUtils.js` | 86 | todo | |
| `js/misc/fileUtils.js` | 86 | todo | |
| `js/ui/animation.js` | 81 | ported | `src/ui/animation.[ch]` (`ShellSpinner`, final; the file only held `Spinner` in 51.1). `new Spinner(16, {hideOnStop: true})` is now `new Shell.Spinner({spinner_size: 16, hide_on_stop: true})` (`size` clashes with `ClutterActor:size`). |
| `js/misc/gnomeSession.js` | 71 | ported | `src/misc/gnome-session.[ch]`: `Shell.gnome_session_{presence,manager,inhibitor}_new` (+ `_finish`, and `_new_sync` for the manager) return gdbus-codegen proxies (`Shell.SessionManagerPresence`, `Shell.SessionManager`, `Shell.SessionManagerInhibitor`); `PresenceStatus`, `InhibitFlags` and `ActionAvailability` are `Shell.*` enums. D-Bus signals are GObject signals (`status-changed`, `inhibitor-added`), methods are `call_*` / `call_*_sync`. |
| `js/misc/dependencies.js` | 68 | dropped | GJS import guards; C links its dependencies. Removed in M6. |
| `js/ui/backgroundMenu.js` | 66 | todo | |
| `js/misc/dbusErrors.js` | 58 | dropped | Maps GJS D-Bus errors; gdbus-codegen handles this in C. Removed in M6. |
| `js/misc/params.js` | 27 | dropped | GJS-only helper (object spread/validation); no C counterpart needed. Removed with GJS in M6. |
| `js/misc/asyncMutex.js` | 24 | dropped | JS promise mutex; not needed in C. Removed in M6. |
| `js/misc/signals.js` | 23 | dropped | GJS EventEmitter shim; C modules use GObject signals. Removed in M6. |
| `js/ui/init.js` | 21 | todo | |
| `js/misc/permissionStore.js` | 18 | ported | `src/misc/permission-store.[ch]`: `Shell.permission_store_new(cancellable)` (promisified in environment.js, with `_finish`) resolves to a gdbus-codegen proxy (`Shell.PermissionStore`, flags NONE so the service is still auto-started and `g_name_owner` is null when that fails). The `LookupAsync/SetAsync/SetPermissionAsync` wrappers are `call_lookup/call_set/call_set_permission(..., null)` taking and returning `GLib.Variant` (`a{sas}`: `permissions.deepUnpack()`; `Set` takes `new GLib.Variant('a{sas}', ...)`), and the `Changed` signal is `changed(table, id, deleted, data, permissions)` instead of `connectSignal`. The proxy is no longer created by a constructor taking a callback: callers await it (weather, status/location, inhibitShortcutsDialog). |
| `js/ui/listModes.js` | 5 | wip | Now prints `Shell.SessionMode.list_modes()`; still the JS entry point that `main.c` evaluates for `--list-modes` (goes with `main.c` in M6). |

## M3

| Upstream module | JS lines | Status | Notes |
|---|---:|---|---|
| `js/ui/appDisplay.js` | 3163 | todo | |
| `js/ui/windowManager.js` | 1987 | todo | |
| `js/ui/iconGrid.js` | 1469 | todo | |
| `js/ui/workspaceThumbnail.js` | 1469 | todo | |
| `js/ui/workspace.js` | 1451 | todo | |
| `js/ui/workspacesView.js` | 1184 | todo | |
| `js/ui/altTab.js` | 1146 | todo | |
| `js/ui/search.js` | 1014 | todo | |
| `js/ui/dash.js` | 1000 | todo | |
| `js/ui/overviewControls.js` | 867 | todo | |
| `js/ui/swipeTracker.js` | 731 | todo | |
| `js/ui/switcherPopup.js` | 718 | todo | |
| `js/ui/windowPreview.js` | 673 | todo | |
| `js/ui/overview.js` | 660 | todo | |
| `js/ui/searchController.js` | 338 | todo | |
| `js/ui/remoteSearch.js` | 334 | todo | |
| `js/ui/appMenu.js` | 292 | todo | |
| `js/ui/runDialog.js` | 271 | todo | |
| `js/ui/windowMenu.js` | 250 | todo | |
| `js/ui/ctrlAltTab.js` | 195 | todo | |
| `js/ui/closeDialog.js` | 179 | todo | |
| `js/ui/appFavorites.js` | 163 | todo | |
| `js/ui/workspaceSwitcherPopup.js` | 128 | ported | `src/ui/workspace-switcher-popup.[ch]` (`ShellWorkspaceSwitcherPopup`, a ClutterActor as before; private monitor constraint reading `MetaDisplay`, since `Layout.MonitorConstraint` is still JS). `new WorkspaceSwitcherPopup()` is now `new Shell.WorkspaceSwitcherPopup({ui_group: Main.uiGroup})`; `display` is `display`. The `ANIMATION_TIME` export and `MonitorWorkspaceSwitcherPopup` class are gone. |
| `js/ui/switchMonitor.js` | 122 | todo | |
| `js/ui/pageIndicators.js` | 115 | ported | `src/ui/page-indicators.[ch]` (`ShellPageIndicators`). `new PageIndicators(orientation)` is now `new Shell.PageIndicators({orientation})`; `setNPages`/`setCurrentPosition` are `set_n_pages`/`set_current_position`; `nPages` is the read-only `n-pages` property; `setReactive` is `set_indicators_reactive` (`set_reactive` is ClutterActor's). The default orientation is still vertical, and the alignment now also follows later orientation changes. |
| `js/ui/checkBox.js` | 48 | ported | `src/ui/check-box.[ch]` (`ShellCheckBox`, final StButton). `new CheckBox(text)` is now `Shell.CheckBox.new(text)`; `setLabel`/`getLabelActor` are `set_label`/`get_label_actor`. No `label` construct property (StButton owns that name). |

## M4

| Upstream module | JS lines | Status | Notes |
|---|---:|---|---|
| `js/ui/screenshot.js` | 3524 | todo | |
| `js/ui/status/network.js` | 2272 | todo | |
| `js/ui/magnifier.js` | 2204 | todo | |
| `js/ui/messageList.js` | 2114 | todo | |
| `js/ui/keyboard.js` | 2093 | todo | |
| `js/ui/lookingGlass.js` | 1775 | todo | |
| `js/ui/messageTray.js` | 1339 | todo | |
| `js/ui/status/keyboard.js` | 1185 | todo | |
| `js/ui/dateMenu.js` | 1007 | todo | |
| `js/ui/padOsd.js` | 1006 | todo | |
| `js/ui/calendar.js` | 917 | todo | |
| `js/ui/quickSettings.js` | 901 | todo | |
| `js/ui/components/networkAgent.js` | 876 | todo | |
| `js/ui/shellMountOperation.js` | 770 | todo | |
| `js/ui/notificationDaemon.js` | 715 | todo | |
| `js/ui/status/volume.js` | 562 | todo | |
| `js/ui/components/polkitAgent.js` | 474 | todo | |
| `js/ui/status/bluetooth.js` | 465 | todo | |
| `js/ui/status/location.js` | 378 | todo | |
| `js/ui/scripting.js` | 369 | todo | |
| `js/ui/status/system.js` | 361 | todo | |
| `js/ui/ibusCandidatePopup.js` | 357 | todo | |
| `js/ui/status/accessibility.js` | 327 | todo | |
| `js/ui/status/thunderbolt.js` | 325 | todo | |
| `js/ui/lightbox.js` | 294 | ported | `src/ui/lightbox.[ch]` (`ShellLightbox`, derivable because Flashspot subclasses it; private `ShellRadialShaderEffect`). `new Lightbox(c, {inhibitEvents, fadeFactor, radialEffect})` is now `new Shell.Lightbox({container: c, inhibit_events, fade_factor, radial_effect})`; `lightOn`/`lightOff` take a mandatory time (`light_on(0)`), `highlight` is `highlight`. The DEFAULT_FADE_FACTOR/VIGNETTE_* exports are gone. |
| `js/ui/components/automountManager.js` | 262 | todo | |
| `js/ui/mpris.js` | 258 | todo | |
| `js/ui/status/backgroundApps.js` | 258 | todo | |
| `js/ui/components/autorunManager.js` | 254 | todo | |
| `js/ui/status/backlight.js` | 242 | todo | |
| `js/ui/components/keyring.js` | 237 | todo | |
| `js/ui/status/remoteAccess.js` | 218 | todo | |
| `js/ui/status/powerProfiles.js` | 152 | todo | |
| `js/ui/status/rfkill.js` | 137 | todo | |
| `js/ui/qrCode.js` | 118 | todo | |
| `js/ui/status/brightness.js` | 101 | todo | |
| `js/ui/focusCaretTracker.js` | 89 | todo | |
| `js/ui/status/dwellClick.js` | 84 | todo | |
| `js/ui/status/nightLight.js` | 69 | todo | |
| `js/ui/components.js` | 60 | todo | |
| `js/ui/status/darkMode.js` | 50 | todo | |
| `js/ui/status/autoRotate.js` | 45 | todo | |
| `js/ui/status/doNotDisturb.js` | 39 | todo | |
| `js/ui/locatePointer.js` | 36 | ported | `src/ui/locate-pointer.[ch]` (`ShellLocatePointer`, final GObject). `new LocatePointer()` is now `new Shell.LocatePointer({ui_group: uiGroup})`; `show()` unchanged. Ripples and settings are released on dispose. |
| `js/ui/status/camera.js` | 19 | todo | |

## M5

| Upstream module | JS lines | Status | Notes |
|---|---:|---|---|
| `js/gdm/loginDialog.js` | 1549 | todo | |
| `js/ui/unlockDialog.js` | 1158 | todo | |
| `js/gdm/authPrompt.js` | 1096 | todo | |
| `js/ui/extensionSystem.js` | 829 | todo | |
| `js/dbusServices/screencast/screencastService.js` | 819 | todo | |
| `js/ui/endSessionDialog.js` | 746 | todo | |
| `js/ui/screenShield.js` | 704 | todo | |
| `js/gdm/authServicesSSSDSwitchable.js` | 643 | todo | |
| `js/gdm/authServices.js` | 537 | todo | |
| `js/gdm/authMenuButton.js` | 519 | todo | |
| `js/gdm/userVerifier.js` | 506 | todo | |
| `js/portalHelper/main.js` | 451 | todo | |
| `js/gdm/authServicesLegacy.js` | 450 | todo | |
| `js/ui/extensionDownloader.js` | 379 | todo | |
| `js/extensions/sharedInternals.js` | 348 | todo | |
| `js/gdm/webLogin.js` | 327 | todo | |
| `js/gdm/authList.js` | 304 | todo | |
| `js/ui/userWidget.js` | 226 | todo | |
| `js/ui/shellEntry.js` | 221 | todo | |
| `js/ui/audioDeviceSelection.js` | 212 | todo | |
| `js/gdm/batch.js` | 204 | todo | |
| `js/dbusServices/dbusService.js` | 202 | todo | |
| `js/ui/accessDialog.js` | 183 | todo | |
| `js/dbusServices/extensions/extensionsService.js` | 175 | todo | |
| `js/gdm/conflictingSessionDialog.js` | 164 | todo | |
| `js/dbusServices/notifications/notificationDaemon.js` | 163 | todo | |
| `js/ui/inhibitShortcutsDialog.js` | 155 | todo | |
| `js/gdm/fingerprintManager.js` | 138 | todo | |
| `js/dbusServices/extensions/extensionPrefsDialog.js` | 132 | todo | |
| `js/gdm/smartcardManager.js` | 125 | todo | |
| `js/extensions/extension.js` | 110 | todo | |
| `js/gdm/realmd.js` | 109 | todo | |
| `js/ui/welcomeDialog.js` | 79 | todo | |
| `js/ui/kbdA11yDialog.js` | 78 | todo | |
| `js/dbusServices/screensaver/screenSaverService.js` | 69 | todo | |
| `js/gdm/fido2TokenManager.js` | 69 | todo | |
| `js/extensions/prefs.js` | 62 | todo | |
| `js/gdm/vmware.js` | 54 | todo | |
| `js/gdm/oVirt.js` | 51 | todo | |
| `js/gdm/util.js` | 39 | todo | |
| `js/gdm/credentialManager.js` | 24 | todo | |
| `js/dbusServices/extensions/main.js` | 19 | todo | |
| `js/gdm/settings.js` | 14 | todo | |
| `js/dbusServices/screencast/main.js` | 13 | todo | |
| `js/dbusServices/notifications/main.js` | 10 | todo | |
| `js/dbusServices/screensaver/main.js` | 10 | todo | |
