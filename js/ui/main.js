import Gio from 'gi://Gio';
import GioUnix from 'gi://GioUnix';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as BreakManager from '../misc/breakManager.js';
import * as Config from '../misc/config.js';
import * as Components from './components.js';
import * as ExtensionSystem from './extensionSystem.js';
import * as ExtensionDownloader from './extensionDownloader.js';
import * as ExtensionUtils from '../misc/extensionUtils.js';
import * as InputSourceManager from './status/keyboard.js';
import * as Overview from './overview.js';
import * as PanelItems from './panelItems.js';
import * as LookingGlass from './lookingGlass.js';
import * as ScreenShield from './screenShield.js';
import {LoginDialog} from '../gdm/loginDialog.js';
import {UnlockDialog} from './unlockDialog.js';
import * as TimeLimitsManager from '../misc/timeLimitsManager.js';
import * as KbdA11yDialog from './kbdA11yDialog.js';
import {formatError} from '../misc/errorUtils.js';
import {ControlsState} from './overviewControls.js';
import * as Util from '../misc/util.js';

const WELCOME_DIALOG_LAST_SHOWN_VERSION = 'welcome-dialog-last-shown-version';
// Make sure to mention the point release, otherwise it will show every time
// until this version is current
const WELCOME_DIALOG_LAST_TOUR_CHANGE = '40.beta';
const LOG_DOMAIN = 'GNOME Shell';
const GNOMESHELL_STARTED_MESSAGE_ID = 'f3ea493c22934e26811cd62abe8e203a';

export let componentManager = null;
export let extensionManager = null;
export let panel = null;
export let overview = null;
export let runDialog = null;
export let lookingGlass = null;
export let welcomeDialog = null;
export let wm = null;
export let messageTray = null;
export let screenShield = null;
export let notificationDaemon = null;
export let windowAttentionHandler = null;
export let ctrlAltTabManager = null;
export let padOsdService = null;
export let osdWindowManager = null;
export let osdMonitorLabeler = null;
export let sessionMode = null;
export let screenshotUI = null;
export let shellAccessDialogDBusService = null;
export let shellAudioSelectionDBusService = null;
export let shellDBusService = null;
export let shellMountOpDBusService = null;
export const screenSaverDBus = null;
export let modalCount = 0;
export let actionMode = Shell.ActionMode.NONE;
export let uiGroup = null;
export let magnifier = null;
export let xdndHandler = null;
export let pointerA11yTimeout = null;
export let keyboard = null;
export let layoutManager = null;
export let kbdA11yDialog = null;
export let inputMethod = null;
export let introspectService = null;
export let locatePointer = null;
export let endSessionDialog = null;
export let breakManager = null;
export let screenTimeDBus = null;
export let breakManagerDispatcher = null;
export let timeLimitsManager = null;
export let timeLimitsDispatcher = null;
export let brightnessManager = null;
export let brightnessDBus = null;

let _startDate;
let _shellExtensionsDBusService = null;
let _screenshotService = null;
let _defaultCssStylesheet = null;
let _cssStylesheet = null;
let _themeResource = null;
let _oskResource = null;
let _iconResource = null;

Gio._promisify(Gio.File.prototype, 'delete_async');
Gio._promisify(Gio.File.prototype, 'touch_async');

let _remoteAccessInhibited = false;

function _sessionUpdated() {
    if (sessionMode.is_primary)
        _loadDefaultStylesheet();

    wm.allow_keybinding('overlay-key',
        Shell.ActionMode.NORMAL | Shell.ActionMode.OVERVIEW);

    wm.allow_keybinding('locate-pointer-key', Shell.ActionMode.ALL);

    wm.set_custom_keybinding_handler('panel-run-dialog',
        Shell.ActionMode.NORMAL | Shell.ActionMode.OVERVIEW,
        sessionMode.has_run_dialog ? openRunDialog : null);

    if (!sessionMode.has_run_dialog) {
        if (runDialog)
            runDialog.close();
        if (lookingGlass)
            lookingGlass.close();
        if (welcomeDialog)
            welcomeDialog.close();
    }

    const remoteAccessController = global.backend.get_remote_access_controller();
    if (remoteAccessController && !global.backend.is_headless()) {
        if (sessionMode.allow_screencast && _remoteAccessInhibited) {
            remoteAccessController.uninhibit_remote_access();
            _remoteAccessInhibited = false;
        } else if (!sessionMode.allow_screencast && !_remoteAccessInhibited) {
            remoteAccessController.inhibit_remote_access();
            _remoteAccessInhibited = true;
        }
    }
}

/** @returns {void} */
export async function start() {
    globalThis.log = console.log;
    globalThis.logError = function (err, msg) {
        const args = [formatError(err)];
        try {
            // toString() can throw
            if (msg)
                args.unshift(`${msg}:`);
        } catch {}

        console.error(...args);
    };

    // Chain up async errors reported from C
    global.connect('notify-error', (global, msg, detail) => {
        notifyError(msg, detail);
    });

    const currentDesktop = GLib.getenv('XDG_CURRENT_DESKTOP');
    if (!currentDesktop || !currentDesktop.split(':').includes('GNOME'))
        GioUnix.DesktopAppInfo.set_desktop_env('GNOME');

    sessionMode = new Shell.SessionMode();
    sessionMode.connect('updated', _sessionUpdated);

    // The popup menus live in C: they follow the session mode, and ask for
    // the JS-only parts of the shell
    _shellMain.session_mode = sessionMode;
    _shellMain.connect('settings-app-activating', () => {
        overview.hide();
        panel.close_quick_settings();
    });

    // The system actions live in C and ask for the JS-only parts of the shell
    const systemActions = Shell.SystemActions.get_default();
    systemActions.session_mode = sessionMode;
    systemActions.connect('lock-screen-requested',
        (_actions, animate) => screenShield?.lock(animate));
    systemActions.connect('hide-overview-requested', () => overview.hide());
    let screenshotOverviewHiddenId = 0;
    systemActions.connect('screenshot-ui-requested', () => {
        if (screenshotOverviewHiddenId)
            return;

        screenshotOverviewHiddenId = overview.connect('hidden', () => {
            overview.disconnect(screenshotOverviewHiddenId);
            screenshotOverviewHiddenId = 0;
            screenshotUI.open(Shell.ScreenshotUIMode.SCREENSHOT);
        });
    });

    St.Settings.get().connect('notify::high-contrast', _loadDefaultStylesheet);
    St.Settings.get().connect('notify::color-scheme', _loadDefaultStylesheet);

    // Initialize ParentalControlsManager before the UI
    Shell.ParentalControlsManager.get_default();

    await _initializeUI();

    shellAccessDialogDBusService = new Shell.AccessDialogDBus();
    shellAudioSelectionDBusService = new Shell.AudioDeviceSelectionDBus();
    shellAudioSelectionDBusService.connect('overview-hide-requested',
        () => overview.hide());
    _createShellDBusServices();
    shellMountOpDBusService = new Shell.MountOpHandler();

    const watchId = Gio.DBus.session.watch_name('org.gnome.Shell.Notifications',
        Gio.BusNameWatcherFlags.AUTO_START,
        bus => bus.unwatch_name(watchId),
        bus => bus.unwatch_name(watchId));

    _sessionUpdated();
}

/**
 * Shows the "Pinned to Dash" / "Unpinned From Dash" notifications of
 * Shell.AppFavorites (js/ui/appFavorites.js had them before it was ported to
 * C), each with an Undo button.
 *
 * @private
 */
function _connectAppFavorites() {
    const favorites = Shell.AppFavorites.get_default();

    const showNotification = (title, undoCallback) => {
        const source = messageTray.get_system_source();
        const notification = new Shell.Notification({
            source,
            title,
            body: null,
            is_transient: true,
            for_feedback: true,
        });
        notification.add_action(_('Undo'), () => undoCallback());
        source.add_notification(notification);
    };

    favorites.connect('favorite-added', (_favorites, app) => {
        const appId = app.get_id();
        showNotification(_('%s Pinned to Dash').format(app.get_name()),
            () => favorites.remove_favorite_silently(appId));
    });
    favorites.connect('favorite-removed', (_favorites, app, pos) => {
        const appId = app.get_id();
        showNotification(_('%s Unpinned From Dash').format(app.get_name()),
            () => favorites.add_favorite_silently(appId, pos));
    });
}

/**
 * Connects the message tray (Shell.MessageTray) to the parts of the shell that
 * are still JS: the tray is hidden from the pick while something is dragged
 * in the overview or from another application, and the keybinding that moves
 * the focus to the banner.
 *
 * @private
 */
function _connectMessageTray() {
    const dragBegin = () => {
        messageTray.dragging = true;
    };
    const dragEnd = () => {
        messageTray.dragging = false;
    };

    overview.connect('window-drag-begin', dragBegin);
    overview.connect('window-drag-cancelled', dragEnd);
    overview.connect('window-drag-end', dragEnd);

    overview.connect('item-drag-begin', dragBegin);
    overview.connect('item-drag-cancelled', dragEnd);
    overview.connect('item-drag-end', dragEnd);

    xdndHandler.connect('drag-begin', dragBegin);
    xdndHandler.connect('drag-end', dragEnd);

    wm.add_keybinding('focus-active-notification',
        new Gio.Settings({schema_id: 'org.gnome.shell.keybindings'}),
        Meta.KeyBindingFlags.NONE,
        Shell.ActionMode.NORMAL | Shell.ActionMode.OVERVIEW,
        () => messageTray.expand_active_notification());
}

/**
 * Exports org.gnome.Shell and org.gnome.Shell.Extensions, and the Screenshot
 * service. The first two live in C (src/ui/shell-dbus.c); what they used to
 * reach through the singletons that are still JS is a signal that is
 * connected here.
 *
 * @private
 */
function _createShellDBusServices() {
    shellDBusService = new Shell.DBusService({
        osd_window_manager: osdWindowManager,
        osd_monitor_labeler: osdMonitorLabeler,
    });

    shellDBusService.connect('eval-requested', async (_service, code, invocation) => {
        // The scripts see the exports of this module as `Main`, as they did
        // when shellDBus.js evaluated them
        // eslint-disable-next-line no-unused-vars
        const Main = await import('./main.js');
        let returnValue;
        let success;
        try {
            returnValue = JSON.stringify(await eval(code));
            // A hack; DBus doesn't have null/undefined
            if (returnValue === undefined)
                returnValue = '';
            success = true;
        } catch (e) {
            returnValue = `${e}`;
            success = false;
        }
        shellDBusService.complete_eval(invocation, success, returnValue);
    });
    shellDBusService.connect('focus-search-requested',
        () => overview.focusSearch());
    shellDBusService.connect('focus-app-requested',
        (_service, id) => overview.selectApp(id));
    shellDBusService.connect('show-applications-requested',
        () => overview.show(ControlsState.APP_GRID));
    shellDBusService.connect('allow-keybinding-requested',
        (_service, name, modeFlags) => wm.allow_keybinding(name, modeFlags));
    shellDBusService.connect('overview-active-requested', (_service, active) => {
        if (active)
            overview.show();
        else
            overview.hide();
    });

    const syncOverviewActive = () => {
        shellDBusService.overview_active = overview.visible;
    };
    overview.connect('showing', syncOverviewActive);
    overview.connect('hidden', syncOverviewActive);

    const extensionsService = new Shell.DBusExtensionsService();
    _shellExtensionsDBusService = extensionsService;

    const serializeExtension = uuid =>
        ExtensionUtils.serializeExtension(extensionManager.lookup(uuid) || {});
    extensionsService.connect('list-extensions', () => {
        const out = {};
        extensionManager.getUuids().forEach(uuid => {
            out[uuid] = serializeExtension(uuid);
        });
        return new GLib.Variant('a{sa{sv}}', out);
    });
    extensionsService.connect('get-extension-info',
        (_service, uuid) => new GLib.Variant('a{sv}', serializeExtension(uuid)));
    extensionsService.connect('get-extension-errors',
        (_service, uuid) => new GLib.Variant('as', extensionManager.lookup(uuid)?.errors ?? []));
    extensionsService.connect('install-remote-extension',
        (_service, uuid, invocation) => ExtensionDownloader.installExtension(uuid, invocation));
    extensionsService.connect('uninstall-extension',
        (_service, uuid) => !!ExtensionDownloader.uninstallExtension(uuid));
    extensionsService.connect('enable-extension',
        (_service, uuid) => !!extensionManager.enableExtension(uuid));
    extensionsService.connect('disable-extension',
        (_service, uuid) => !!extensionManager.disableExtension(uuid));
    extensionsService.connect('open-extension-prefs',
        (_service, uuid, parentWindow, options) =>
            extensionManager.openExtensionPrefs(uuid, parentWindow, options.deepUnpack()));
    extensionsService.connect('check-for-updates',
        () => ExtensionDownloader.checkForUpdates());
    extensionManager.connect('extension-state-changed', (_manager, extension) => {
        extensionsService.emit_state_changed(extension.uuid,
            new GLib.Variant('a{sv}', ExtensionUtils.serializeExtension(extension)),
            extension.state, extension.error ?? '');
    });

    _screenshotService = new Shell.ScreenshotService({screenshot_ui: screenshotUI});
}

/** @private */
async function _initializeUI() {
    // Ensure ShellWindowTracker and ShellAppUsage are initialized; this will
    // also initialize ShellAppSystem first. ShellAppSystem
    // needs to load all the .desktop files, and ShellWindowTracker
    // will use those to associate with windows. Right now
    // the Monitor doesn't listen for installed app changes
    // and recalculate application associations, so to avoid
    // races for now we initialize it here. It's better to
    // be predictable anyways.
    Shell.WindowTracker.get_default();
    Shell.AppUsage.get_default();

    reloadThemeResource();
    _loadIcons();
    _loadOskLayouts();
    _loadDefaultStylesheet();

    new AnimationsSettings();

    // Setup the stage hierarchy early
    layoutManager = new Shell.LayoutManager();
    _shellMain.layout_manager = layoutManager;

    // The layout manager lives in C: it asks for the JS-only parts of the shell
    layoutManager.connect('background-added', (_layoutManager, bgActor) => {
        Shell.BackgroundMenu.add_to(bgActor, layoutManager);
    });
    layoutManager.connect('hot-corner-activated', (_layoutManager, _monitorIndex, inFullscreen) => {
        if (!overview || (inFullscreen && !overview.visible))
            return false;

        if (!overview.shouldToggleByCornerOrButton())
            return false;

        overview.toggle();
        return overview.animationInProgress;
    });
    layoutManager.connect('startup-animation-requested', () => {
        overview.runStartupAnimation()
            .catch(logError)
            .finally(() => layoutManager.startup_animation_finished());
        return true;
    });

    // The signal is emitted from C (modal dialogs, the screenshot UI) and from JS:
    // forward it both ways, once
    let forwardingSystemModalOpened = false;
    layoutManager.connect('system-modal-opened', () => {
        if (forwardingSystemModalOpened)
            return;
        forwardingSystemModalOpened = true;
        _shellMain.system_modal_opened();
        forwardingSystemModalOpened = false;
    });
    _shellMain.connect('system-modal-opened', () => {
        if (forwardingSystemModalOpened)
            return;
        forwardingSystemModalOpened = true;
        layoutManager.emit('system-modal-opened');
        forwardingSystemModalOpened = false;
    });

    // The modal dialogs live in C: they are added to the layout manager's
    // modal dialog group
    Shell.ModalDialog.set_default_container(layoutManager.modal_dialog_group);

    // Various parts of the codebase still refer to Main.uiGroup
    // instead of using the layoutManager. This keeps that code
    // working until it's updated.
    uiGroup = layoutManager.ui_group;

    padOsdService = new Shell.PadOsdService();
    xdndHandler = new Shell.XdndHandler({
        ui_group: uiGroup,
        session_locked: sessionMode.is_locked,
    });
    Shell.dnd_set_xdnd_handler(xdndHandler);
    sessionMode.connect('updated', () => {
        xdndHandler.session_locked = sessionMode.is_locked;
    });
    ctrlAltTabManager = new Shell.CtrlAltTabManager();
    // What Main.activateWindow() does on top of activating the window
    ctrlAltTabManager.connect('window-activated', () => {
        overview.hide();
        panel.close_calendar();
    });
    osdWindowManager = new Shell.OsdWindowManager({ui_group: uiGroup});
    _shellMain.osd_window_manager = osdWindowManager;
    osdMonitorLabeler = new Shell.OsdMonitorLabeler({ui_group: uiGroup});
    overview = new Overview.Overview();
    kbdA11yDialog = new KbdA11yDialog.KbdA11yDialog();
    wm = new Shell.WindowManager({ctrl_alt_tab_manager: ctrlAltTabManager});
    _connectWindowManager();
    magnifier = new Shell.Magnifier();
    locatePointer = new Shell.LocatePointer({ui_group: uiGroup});

    if (Shell.LoginManager.can_lock())
        screenShield = new ScreenShield.ScreenShield();

    inputMethod = new Shell.InputMethod();
    global.stage.context.get_backend().set_input_method(inputMethod);
    global.connect('shutdown',
        () => global.stage.context.get_backend().set_input_method(null));

    screenshotUI = new Shell.ScreenshotUI({window_manager: wm});

    brightnessManager = new Shell.BrightnessManager();
    for (const name of Shell.BrightnessManager.get_keybinding_names())
        wm.allow_keybinding(name, Shell.ActionMode.ALL);
    brightnessManager.connect('osd-requested', (_manager, icon, levels) => {
        osdWindowManager.show(icon, null, levels);
    });
    brightnessDBus = new Shell.BrightnessService({manager: brightnessManager});

    messageTray = new Shell.MessageTray();
    _shellMain.message_tray = messageTray;
    _connectMessageTray();
    panel = _createPanel();
    _shellMain.panel = panel;
    keyboard = new Shell.OskManager({input_method: inputMethod});
    InputSourceManager.connectOsk(keyboard);
    notificationDaemon = new Shell.NotificationDaemon({message_tray: messageTray});
    // What the notification sources did on top of activating an application
    notificationDaemon.connect('app-opened', () => {
        overview.hide();
        panel.close_calendar();
    });
    windowAttentionHandler = new Shell.WindowAttentionHandler({message_tray: messageTray});
    // What Main.activateWindow() does on top of activating the window
    windowAttentionHandler.connect('window-activated', () => {
        overview.hide();
        panel.close_calendar();
    });
    componentManager = new Components.ComponentManager();

    introspectService = new Shell.IntrospectService();

    // Set up the global default break reminder manager and its D-Bus interface
    breakManager = new BreakManager.BreakManager();
    timeLimitsManager = new TimeLimitsManager.TimeLimitsManager();
    screenTimeDBus = new Shell.ScreenTimeService({manager: breakManager});
    breakManagerDispatcher = new BreakManager.BreakDispatcher(breakManager);
    timeLimitsDispatcher = new TimeLimitsManager.TimeLimitsDispatcher(timeLimitsManager);

    global.connect('shutdown', () => {
        // Block shutdown until the session history file has been written
        const loop = new GLib.MainLoop(null, false);
        const source = GLib.idle_source_new();
        source.set_callback(() => {
            timeLimitsManager.shutdown()
                .catch(e => console.warn(`Failed to stop time limits manager: ${e.message}`))
                .finally(() => loop.quit());
            return GLib.SOURCE_REMOVE;
        });
        source.attach(loop.get_context());
        loop.run();
    });

    layoutManager.begin_startup();
    overview.init();
    _connectAppFavorites();

    pointerA11yTimeout = new Shell.PointerA11yTimeout({ui_group: uiGroup});

    global.connect('locate-pointer', () => {
        locatePointer.show();
    });

    global.context.connect('notify::unsafe-mode', () => {
        if (!global.context.unsafe_mode)
            return; // we're safe
        if (lookingGlass?.isOpen)
            return; // assume user action

        const source = messageTray.get_system_source();
        const notification = new Shell.Notification({
            source,
            title: _('System was put in unsafe mode'),
            body: _('Apps now have unrestricted access'),
            is_transient: true,
        });
        notification.add_action(_('Undo'),
            () => (global.context.unsafe_mode = false));
        source.add_notification(notification);
    });

    // Provide the bus object for gnome-session to
    // initiate logouts.
    endSessionDialog = new Shell.EndSessionDialog();

    // We're ready for the session manager to move to the next phase
    GLib.idle_add_once(GLib.PRIORITY_DEFAULT, () => {
        Shell.util_sd_notify();
        global.context.notify_ready();
    });

    _startDate = new Date();

    ExtensionDownloader.init();
    extensionManager = new ExtensionSystem.ExtensionManager();
    extensionManager.init();

    Shell.LoginManager.register_session_with_gdm();

    if (sessionMode.is_greeter && screenShield) {
        layoutManager.connect('startup-prepared', () => {
            screenShield.showDialog();
        });
    }

    let Scripting;
    let perfModule;
    const {automationScript} = global;
    if (automationScript) {
        Scripting = await import('./scripting.js');
        perfModule = await import(automationScript.get_uri());
        if (perfModule.init)
            perfModule.init();
    }

    layoutManager.connect('startup-complete', () => {
        if (actionMode === Shell.ActionMode.NONE)
            _shellMain.action_mode = Shell.ActionMode.NORMAL;

        // Lua extensions (gnome-lua): emits the "shell.started" hook
        global.notify_startup_complete();

        if (screenShield)
            screenShield.lockIfWasLocked();

        if (sessionMode.current_mode !== 'gdm' &&
            sessionMode.current_mode !== 'initial-setup') {
            GLib.log_structured(LOG_DOMAIN, GLib.LogLevelFlags.LEVEL_MESSAGE, {
                'MESSAGE': `GNOME Shell started at ${_startDate}`,
                'MESSAGE_ID': GNOMESHELL_STARTED_MESSAGE_ID,
            });
        }

        if (!perfModule) {
            const credentials = new Gio.Credentials();
            if (credentials.get_unix_user() === 0) {
                notify(
                    _('Logged in as a privileged user'),
                    _('Running a session as a privileged user should be avoided for security reasons. If possible, you should log in as a normal user.'));
            } else if (sessionMode.show_welcome_dialog) {
                _handleShowWelcomeScreen();
            }
        }

        if (sessionMode.current_mode !== 'gdm' &&
            sessionMode.current_mode !== 'initial-setup')
            _handleLockScreenWarning();

        Shell.LoginManager.register_display_with_gdm();

        if (perfModule) {
            const perfOutput = GLib.getenv('SHELL_PERF_OUTPUT');
            Scripting.runPerfScript(perfModule, perfOutput);
        }
    });
}

/**
 * Wires up the window manager, which lives in C, to the JS-only parts of the
 * shell: it asks the overview to hide, and for a screenshot of a window, and
 * it follows the state of the overview.
 *
 * @private
 */
function _connectWindowManager() {
    wm.connect('overview-hide-requested', () => {
        overview.hide();
    });
    wm.connect('screenshot-requested', (_wm, texture) => {
        Shell.Screenshot.capture_screenshot_async(
            texture, null, 1, null, 0, 0, 1, Shell.ScreenshotKind.WINDOW,
            null, (_source, res) => {
                try {
                    Shell.Screenshot.capture_screenshot_finish(res);
                } catch (e) {
                    logError(e, 'Error capturing screenshot');
                }
            });
    });

    // Handlers must not return a value: a true one stops the JS emission
    overview.connect('showing', () => {
        wm.overview_closing = false;
        wm.overview_visible = true;
    });
    overview.connect('shown', () => {
        wm.overview_closing = false;
    });
    overview.connect('hiding', () => {
        wm.overview_closing = true;
    });
    overview.connect('hidden', () => {
        wm.overview_closing = false;
        wm.overview_visible = false;
    });
}

/**
 * Creates the top bar. It lives in C; the indicators of the shell are JS
 * classes (panelItems.js) that it asks for through its create-indicator
 * signal, and the parts of the shell it needs but cannot reach (the overview,
 * the message tray, the Ctrl+Alt+Tab popup) are wired up here.
 *
 * @returns {Shell.Panel} the new panel
 */
function _createPanel() {
    const newPanel = new Shell.Panel();

    newPanel.connect('create-indicator',
        (_panel, role) => PanelItems.createPanelItem(role));

    ctrlAltTabManager.add_group_full(newPanel,
        _('Top Bar'), 'shell-focus-top-bar-symbolic',
        Shell.CtrlAltTabSortGroup.TOP, null, null);

    overview.connectObject(
        // A handler returning true stops the JS signal emission, so
        // never return the assigned value here
        'showing', () => {
            newPanel.overview_visible = true;
        },
        'hiding', () => {
            newPanel.overview_visible = false;
        },
        newPanel);

    // Banners are aligned with the date menu, and hidden while a menu is
    // open in the same box of the top bar
    newPanel.connect('notify::banner-alignment', () => {
        messageTray.banner_alignment = newPanel.banner_alignment;
    });
    newPanel.connect('notify::banner-blocked', () => {
        messageTray.banner_blocked = newPanel.banner_blocked;
    });

    newPanel.begin_startup();
    messageTray.banner_alignment = newPanel.banner_alignment;

    return newPanel;
}

function _handleShowWelcomeScreen() {
    const lastShownVersion = global.settings.get_string(WELCOME_DIALOG_LAST_SHOWN_VERSION);
    if (Util.GNOMEversionCompare(WELCOME_DIALOG_LAST_TOUR_CHANGE, lastShownVersion) > 0) {
        openWelcomeDialog();
        global.settings.set_string(WELCOME_DIALOG_LAST_SHOWN_VERSION, Config.PACKAGE_VERSION);
    }
}

async function _handleLockScreenWarning() {
    const path = `${global.userdatadir}/lock-warning-shown`;
    const file = Gio.File.new_for_path(path);

    const hasLockScreen = screenShield !== null;
    if (hasLockScreen) {
        try {
            await file.delete_async(0, null);
        } catch (e) {
            if (!e.matches(Gio.IOErrorEnum, Gio.IOErrorEnum.NOT_FOUND))
                logError(e);
        }
    } else {
        try {
            if (!await file.touch_async())
                return;
        } catch (e) {
            logError(e);
        }

        notify(
            _('Screen Lock disabled'),
            _('Screen Locking requires the GNOME display manager'));
    }
}

function _getStylesheet(name) {
    let stylesheet;

    stylesheet = Gio.File.new_for_uri(`resource:///org/gnome/shell/theme/${name}`);
    if (stylesheet.query_exists(null))
        return stylesheet;

    const dataDirs = GLib.get_system_data_dirs();
    for (let i = 0; i < dataDirs.length; i++) {
        const path = GLib.build_filenamev([dataDirs[i], 'gnome-shell', 'theme', name]);
        stylesheet = Gio.file_new_for_path(path);
        if (stylesheet.query_exists(null))
            return stylesheet;
    }

    stylesheet = Gio.File.new_for_path(`${global.datadir}/theme/${name}`);
    if (stylesheet.query_exists(null))
        return stylesheet;

    return null;
}

/**
 * Maps Shell.SessionMode:unlock-dialog to the class that implements it.
 *
 * @returns {?Function} the dialog constructor, null if the mode has none
 */
export function getUnlockDialogClass() {
    switch (sessionMode.unlock_dialog) {
    case 'login-dialog':
        return LoginDialog;
    case 'unlock-dialog':
        return UnlockDialog;
    default:
        return null;
    }
}

/** @returns {string} */
export function getStyleVariant() {
    const {colorScheme} = St.Settings.get();
    switch (sessionMode.color_scheme) {
    case 'force-dark':
        return 'dark';
    case 'force-light':
        return 'light';
    case 'prefer-dark':
        return colorScheme === St.SystemColorScheme.PREFER_LIGHT
            ? 'light' : 'dark';
    case 'prefer-light':
        return colorScheme === St.SystemColorScheme.PREFER_DARK
            ? 'dark' : 'light';
    default:
        return '';
    }
}

function _getDefaultStylesheet() {
    let stylesheet = null;
    const name = sessionMode.stylesheet_name;

    // Look for a high-contrast variant first
    if (St.Settings.get().high_contrast)
        stylesheet = _getStylesheet(name.replace('.css', '-high-contrast.css'));

    if (stylesheet === null)
        stylesheet = _getStylesheet(name.replace('.css', `-${getStyleVariant()}.css`));

    if (stylesheet == null)
        stylesheet = _getStylesheet(name);

    return stylesheet;
}

function _loadDefaultStylesheet() {
    const stylesheet = _getDefaultStylesheet();
    if (_defaultCssStylesheet && _defaultCssStylesheet.equal(stylesheet))
        return;

    _defaultCssStylesheet = stylesheet;
    loadTheme();
}

/**
 * Creates an adjustment that has its lower, upper, and value
 * properties set for the number of available workspaces. Consumers
 * of the returned adjustment must only change the 'value' property,
 * and only that. The master adjustment lives in the window manager.
 *
 * @param {Clutter.Actor} actor
 *
 * @returns {St.Adjustment} - an adjustment representing the
 * current workspaces layout
 */
export function createWorkspacesAdjustment(actor) {
    return wm.create_workspaces_adjustment(actor);
}

/**
 * Get the theme CSS file that the shell will load
 *
 * @returns {?Gio.File}: A #GFile that contains the theme CSS,
 *          null if using the default
 */
export function getThemeStylesheet() {
    return _cssStylesheet;
}

/**
 * Set the theme CSS file that the shell will load
 *
 * @param {string=} cssStylesheet - A file path that contains the theme CSS,
 *     set it to null to use the default
 */
export function setThemeStylesheet(cssStylesheet) {
    _cssStylesheet = cssStylesheet ? Gio.File.new_for_path(cssStylesheet) : null;
}

export function reloadThemeResource() {
    if (_themeResource)
        _themeResource._unregister();

    _themeResource = Gio.Resource.load(
        `${global.datadir}/${sessionMode.theme_resource_name}`);
    _themeResource._register();
}

/** @private */
function _loadIcons() {
    _iconResource = Gio.Resource.load(`${global.datadir}/gnome-shell-icons.gresource`);
    _iconResource._register();
}

function _loadOskLayouts() {
    _oskResource = Gio.Resource.load(`${global.datadir}/gnome-shell-osk-layouts.gresource`);
    _oskResource._register();
}

/**
 * loadTheme:
 *
 * Reloads the theme CSS file
 */
export function loadTheme() {
    const themeContext = St.ThemeContext.get_for_stage(global.stage);
    const previousTheme = themeContext.get_theme();

    const theme = new St.Theme({
        application_stylesheet: _cssStylesheet,
        default_stylesheet: _defaultCssStylesheet,
    });

    if (theme.default_stylesheet == null)
        throw new Error(`No valid stylesheet found for '${sessionMode.stylesheet_name}'`);

    if (previousTheme) {
        const customStylesheets = previousTheme.get_custom_stylesheets();

        for (let i = 0; i < customStylesheets.length; i++)
            theme.load_stylesheet(customStylesheets[i]);
    }

    themeContext.set_theme(theme);
}

/**
 * @param {string} msg A message
 * @param {string=} details Additional information
 */
export function notify(msg, details = null) {
    const source = messageTray.get_system_source();
    const notification = new Shell.Notification({
        source,
        title: msg,
        body: details,
        is_transient: true,
    });
    source.add_notification(notification);
}

/**
 * See shell_global_notify_problem().
 *
 * @param {string} msg - An error message
 * @param {string} details - Additional information
 */
export function notifyError(msg, details) {
    // Also print to stderr so it's logged somewhere
    if (details)
        console.warn(`error: ${msg}: ${details}`);
    else
        console.warn(`error: ${msg}`);

    notify(msg, details);
}

// The modal stack lives in C (Shell.Main); modalCount and actionMode mirror
// its properties, which are notified synchronously at every change.
const _shellMain = Shell.Main.get_default();
modalCount = _shellMain.modal_count;
actionMode = _shellMain.action_mode;
_shellMain.connect('notify::modal-count', () => {
    modalCount = _shellMain.modal_count;
});
_shellMain.connect('notify::action-mode', () => {
    actionMode = _shellMain.action_mode;
});
_shellMain.connect('modal-ended', () => layoutManager.modal_ended());

/**
 * Ensure we are in a mode where all keyboard and mouse input goes to
 * the stage, and focus @actor. Multiple calls to this function act in
 * a stacking fashion; the effect will be undone when an equal number
 * of popModal() invocations have been made.
 *
 * Next, record the current Clutter keyboard focus on a stack. If the
 * modal stack returns to this actor, reset the focus to the actor
 * which was focused at the time pushModal() was invoked.
 *
 * `params` may be used to provide the following parameters:
 *  - actionMode: used to set the current Shell.ActionMode to filter
 *                global keybindings; the default of NONE will filter
 *                out all keybindings
 *
 * @param {Clutter.Actor} actor - actor which will be given keyboard focus
 * @param {object=} params - optional parameters
 * @returns {Clutter.Grab} - the grab handle created
 */
export function pushModal(actor, params = {}) {
    const {actionMode: newActionMode} = {
        actionMode: Shell.ActionMode.NONE,
        ...params,
    };

    return _shellMain.push_modal(actor, newActionMode);
}

/**
 * Reverse the effect of pushModal(). If this invocation is undoing
 * the topmost invocation, then the focus will be restored to the
 * previous focus at the time when pushModal() was invoked.
 *
 * @param {Clutter.Grab} grab - the grab given by pushModal()
 */
export function popModal(grab) {
    // Throws "incorrect pop" if the grab is not on the modal stack
    _shellMain.pop_modal(grab);
}

/**
 * Creates the looking glass panel
 *
 * @returns {LookingGlass.LookingGlass}
 */
export function createLookingGlass() {
    if (lookingGlass == null)
        lookingGlass = new LookingGlass.LookingGlass();

    return lookingGlass;
}

/**
 * Opens the run dialog
 */
export function openRunDialog() {
    if (runDialog == null) {
        runDialog = new Shell.RunDialog();
        // The internal commands of the development tools which need JS code
        runDialog.connect('internal-command', (dialog, name) => {
            switch (name) {
            case 'lg':
                createLookingGlass().open();
                return true;
            case 'rt':
                // rt is short for "reload theme"
                reloadThemeResource();
                loadTheme();
                return true;
            default:
                return false;
            }
        });
    }

    runDialog.open();
}

export function openWelcomeDialog() {
    if (welcomeDialog === null) {
        welcomeDialog = new Shell.WelcomeDialog();
        welcomeDialog.connect('overview-hide-requested', () => {
            overview.hide();
        });
    }

    welcomeDialog.open();
}

/**
 * activateWindow:
 *
 * @param {Meta.Window} window the window to activate
 * @param {number=} time current event time
 * @param {number=} workspaceNum  window's workspace number
 *
 * Activates @window, switching to its workspace first if necessary,
 * and switching out of the overview if it's currently active
 */
export function activateWindow(window, time, workspaceNum) {
    const workspaceManager = global.workspace_manager;
    const activeWorkspaceNum = workspaceManager.get_active_workspace_index();
    const windowWorkspaceNum = workspaceNum !== undefined ? workspaceNum : window.get_workspace().index();

    if (!time)
        time = global.get_current_time();

    if (windowWorkspaceNum !== activeWorkspaceNum) {
        const workspace = workspaceManager.get_workspace_by_index(windowWorkspaceNum);
        workspace.activate_with_focus(window, time);
    } else {
        window.activate(time);
    }

    overview.hide();
    panel.close_calendar();
}

/**
 * Move @window to the specified monitor and workspace.
 *
 * @param {Meta.Window} window - the window to move
 * @param {number} monitorIndex - the requested monitor
 * @param {number} workspaceIndex - the requested workspace
 * @param {bool} append - create workspace if it doesn't exist
 */
export function moveWindowToMonitorAndWorkspace(window, monitorIndex, workspaceIndex, append = false) {
    // We need to move the window before changing the workspace, because
    // the move itself could cause a workspace change if the window enters
    // the primary monitor
    if (window.get_monitor() !== monitorIndex) {
        // Wait for the monitor change to take effect
        const id = global.display.connect('window-entered-monitor',
            (dsp, num, w) => {
                if (w !== window)
                    return;
                window.change_workspace_by_index(workspaceIndex, append);
                global.display.disconnect(id);
            });
        window.move_to_monitor(monitorIndex);
    } else {
        window.change_workspace_by_index(workspaceIndex, append);
    }
}

// TODO - replace this timeout with some system to guess when the user might
// be e.g. just reading the screen and not likely to interact.
const DEFERRED_TIMEOUT_SECONDS = 20;
const _deferredWorkData = {};
// Work scheduled for some point in the future
const _deferredWorkQueue = [];
// Work we need to process before the next redraw
let _beforeRedrawQueue = [];
// Counter to assign work ids
let _deferredWorkSequence = 0;
let _deferredTimeoutId = 0;

function _runDeferredWork(workId) {
    if (!_deferredWorkData[workId])
        return;
    const index = _deferredWorkQueue.indexOf(workId);
    if (index < 0)
        return;

    _deferredWorkQueue.splice(index, 1);
    _deferredWorkData[workId].callback();
    if (_deferredWorkQueue.length === 0 && _deferredTimeoutId > 0) {
        GLib.source_remove(_deferredTimeoutId);
        _deferredTimeoutId = 0;
    }
}

function _runAllDeferredWork() {
    while (_deferredWorkQueue.length > 0)
        _runDeferredWork(_deferredWorkQueue[0]);
}

function _runBeforeRedrawQueue() {
    for (let i = 0; i < _beforeRedrawQueue.length; i++) {
        const workId = _beforeRedrawQueue[i];
        _runDeferredWork(workId);
    }
    _beforeRedrawQueue = [];
}

function _queueBeforeRedraw(workId) {
    _beforeRedrawQueue.push(workId);
    if (_beforeRedrawQueue.length === 1) {
        const laters = global.compositor.get_laters();
        laters.add(Meta.LaterType.BEFORE_REDRAW, () => {
            _runBeforeRedrawQueue();
            return GLib.SOURCE_REMOVE;
        });
    }
}

/**
 * This function sets up a callback to be invoked when either the
 * given actor is mapped, or after some period of time when the machine
 * is idle. This is useful if your actor isn't always visible on the
 * screen (for example, all actors in the overview), and you don't want
 * to consume resources updating if the actor isn't actually going to be
 * displaying to the user.
 *
 * Note that queueDeferredWork is called by default immediately on
 * initialization as well, under the assumption that new actors
 * will need it.
 *
 * @param {Clutter.Actor} actor - an actor
 * @param {callback} callback - Function to invoke to perform work
 *
 * @returns {string} - A string work identifier
 */
export function initializeDeferredWork(actor, callback) {
    // Turn into a string so we can use as an object property
    const workId = `${++_deferredWorkSequence}`;
    _deferredWorkData[workId] = {
        actor,
        callback,
    };
    actor.connect('notify::mapped', () => {
        if (!(actor.mapped && _deferredWorkQueue.includes(workId)))
            return;
        _queueBeforeRedraw(workId);
    });
    actor.connect('destroy', () => {
        const index = _deferredWorkQueue.indexOf(workId);
        if (index >= 0)
            _deferredWorkQueue.splice(index, 1);
        delete _deferredWorkData[workId];
    });
    queueDeferredWork(workId);
    return workId;
}

/**
 * queueDeferredWork:
 *
 * @param {string} workId work identifier
 *
 * Ensure that the work identified by @workId will be
 * run on map or timeout. You should call this function
 * for example when data being displayed by the actor has
 * changed.
 */
export function queueDeferredWork(workId) {
    const data = _deferredWorkData[workId];
    if (!data) {
        const message = `Invalid work id ${workId}`;
        logError(new Error(message), message);
        return;
    }
    if (!_deferredWorkQueue.includes(workId))
        _deferredWorkQueue.push(workId);
    if (data.actor.mapped) {
        _queueBeforeRedraw(workId);
    } else if (_deferredTimeoutId === 0) {
        _deferredTimeoutId = GLib.timeout_add_seconds_once(GLib.PRIORITY_DEFAULT, DEFERRED_TIMEOUT_SECONDS, () => {
            _runAllDeferredWork();
            _deferredTimeoutId = 0;
        });
        GLib.Source.set_name_by_id(_deferredTimeoutId, '[gnome-shell] _runAllDeferredWork');
    }
}

class AnimationsSettings {
    constructor() {
        this._animationsEnabled = true;
        this._handles = new Set();

        global.connect('notify::force-animations',
            this._syncAnimationsEnabled.bind(this));
        this._syncAnimationsEnabled();

        const backend = global.backend;
        const remoteAccessController = backend.get_remote_access_controller();
        if (remoteAccessController) {
            remoteAccessController.connect('new-handle',
                (_, handle) => this._onNewRemoteAccessHandle(handle));
        }
    }

    _shouldEnableAnimations() {
        if (global.force_animations)
            return true;

        if (this._handles.size > 0)
            return false;

        const backend = global.backend;
        if (!backend.is_rendering_hardware_accelerated())
            return false;

        if (Shell.util_has_x11_display_extension(
            global.display, 'VNC-EXTENSION'))
            return false;

        return true;
    }

    _syncAnimationsEnabled() {
        const shouldEnableAnimations = this._shouldEnableAnimations();
        if (this._animationsEnabled === shouldEnableAnimations)
            return;
        this._animationsEnabled = shouldEnableAnimations;

        const settings = St.Settings.get();
        if (shouldEnableAnimations)
            settings.uninhibit_animations();
        else
            settings.inhibit_animations();
    }

    _onRemoteAccessHandleStopped(handle) {
        this._handles.delete(handle);
        this._syncAnimationsEnabled();
    }

    _onNewRemoteAccessHandle(handle) {
        if (!handle.get_disable_animations())
            return;

        this._handles.add(handle);
        this._syncAnimationsEnabled();
        handle.connect('stopped', this._onRemoteAccessHandleStopped.bind(this));
    }
}
