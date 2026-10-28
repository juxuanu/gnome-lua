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
| `js/ui/popupMenu.js` | 1683 | todo | |
| `js/misc/breakManager.js` | 1480 | todo | |
| `js/misc/timeLimitsManager.js` | 1475 | todo | |
| `js/ui/layout.js` | 1430 | todo | |
| `js/ui/main.js` | 1069 | todo | |
| `js/ui/background.js` | 997 | todo | |
| `js/ui/panel.js` | 786 | todo | |
| `js/ui/boxpointer.js` | 689 | ported | `src/ui/box-pointer.[ch]` (`ShellBoxPointer`, derivable; `ShellPopupAnimation` flags). `new BoxPointer(side)` is now `new Shell.BoxPointer({arrow_side: side})`; methods are snake_case (`set_position`, `set_arrow_origin`, `open(animate, onComplete)`, ...), `.arrowSide` is `.arrow_side`, `BoxPointer.PopupAnimation` is `Shell.PopupAnimation`. `bin` is a read-only property. New `arrow-alignment`/`arrow-actor` properties. Monitor/work area come straight from MetaDisplay (workspace 0), falling back to the primary monitor for an off-screen source. |
| `js/ui/dnd.js` | 643 | todo | |
| `js/ui/shellDBus.js` | 634 | todo | |
| `js/ui/workspaceAnimation.js` | 599 | todo | |
| `js/misc/brightnessManager.js` | 551 | todo | |
| `js/misc/systemActions.js` | 525 | todo | |
| `js/misc/inputMethod.js` | 480 | todo | |
| `js/misc/util.js` | 471 | todo | |
| `js/ui/environment.js` | 464 | wip | ease helpers in C; remaining JS is the GJS bootstrap, deleted in M6 |
| `js/misc/ibusManager.js` | 372 | todo | |
| `js/ui/dialog.js` | 352 | todo | |
| `js/misc/weather.js` | 327 | todo | |
| `js/misc/keyboardManager.js` | 309 | todo | |
| `js/misc/jsParse.js` | 302 | todo | |
| `js/misc/modemManager.js` | 302 | todo | |
| `js/misc/signalTracker.js` | 287 | todo | |
| `js/misc/loginManager.js` | 279 | todo | |
| `js/ui/barLevel.js` | 271 | ported | `src/ui/bar-level.[ch]` (`ShellBarLevel`, derivable). `overdrive-start` above the maximum now logs a warning instead of throwing. |
| `js/misc/parentalControlsManager.js` | 267 | todo | |
| `js/ui/modalDialog.js` | 266 | todo | |
| `js/ui/grabHelper.js` | 242 | todo | |
| `js/misc/objectManager.js` | 241 | todo | |
| `js/misc/dateUtils.js` | 230 | todo | |
| `js/ui/slider.js` | 230 | ported | `src/ui/slider.[ch]` (`ShellSlider`, final). `new Slider(v)` is now `new Shell.Slider({value: v})`. |
| `js/misc/introspect.js` | 218 | todo | |
| `js/ui/sessionMode.js` | 209 | todo | |
| `js/ui/osdWindow.js` | 199 | todo | |
| `js/ui/panelMenu.js` | 199 | todo | |
| `js/misc/extensionUtils.js` | 172 | todo | |
| `js/ui/pointerA11yTimeout.js` | 136 | ported | `src/ui/pointer-a11y-timeout.[ch]` (`ShellPointerA11yTimeout`, final; the pie timer actor is private to the C file). main.js now does `Main.pointerA11yTimeout = new Shell.PointerA11yTimeout({ui_group: uiGroup})` and must keep the reference (the JS class lived on through its seat signal closures). |
| `js/ui/xdndHandler.js` | 127 | ported | `src/ui/xdnd-handler.[ch]` (`ShellXdndHandler`, final). Signals `drag-begin (time)`, `drag-end`, new `drag-motion (x, y, drag-actor, target-actor)`; the drag-monitor / `_delegate.handleDragOver` dispatch is now `DND.handleXdndMotion()` in dnd.js, wired in main.js. `Main.sessionMode.isLocked` is mirrored into the `session-locked` property by main.js. |
| `js/misc/animationUtils.js` | 124 | wip | adjustAnimationTime in C |
| `js/ui/osdMonitorLabeler.js` | 119 | todo | |
| `js/ui/ripples.js` | 112 | ported | `src/ui/ripples.[ch]` (`ShellRipples`, final GObject). `new Ripples(px, py, cls, req)` is now `Shell.Ripples.new(px, py, cls, req)`; `addTo`/`playAnimation` are `add_to`/`play_animation`. Calling `add_to` twice or `play_animation` before `add_to` logs a critical instead of throwing. |
| `js/misc/history.js` | 111 | todo | |
| `js/misc/errorUtils.js` | 91 | todo | |
| `js/ui/windowAttentionHandler.js` | 91 | todo | blocked on messageTray (M4): ~85% of the file is the `MessageTray.Source`/`Notification` glue, the C-able signal filter is about ten lines |
| `js/misc/dbusUtils.js` | 86 | todo | |
| `js/misc/fileUtils.js` | 86 | todo | |
| `js/ui/animation.js` | 81 | ported | `src/ui/animation.[ch]` (`ShellSpinner`, final; the file only held `Spinner` in 51.1). `new Spinner(16, {hideOnStop: true})` is now `new Shell.Spinner({spinner_size: 16, hide_on_stop: true})` (`size` clashes with `ClutterActor:size`). |
| `js/misc/gnomeSession.js` | 71 | todo | |
| `js/misc/dependencies.js` | 68 | todo | |
| `js/ui/backgroundMenu.js` | 66 | todo | |
| `js/misc/dbusErrors.js` | 58 | todo | |
| `js/misc/params.js` | 27 | todo | |
| `js/misc/asyncMutex.js` | 24 | todo | |
| `js/misc/signals.js` | 23 | todo | |
| `js/ui/init.js` | 21 | todo | |
| `js/misc/permissionStore.js` | 18 | todo | |
| `js/ui/listModes.js` | 5 | todo | |

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
| `js/ui/workspaceSwitcherPopup.js` | 128 | todo | |
| `js/ui/switchMonitor.js` | 122 | todo | |
| `js/ui/pageIndicators.js` | 115 | todo | |
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
