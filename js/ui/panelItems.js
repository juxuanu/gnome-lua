import Atk from 'gi://Atk';
import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Graphene from 'gi://Graphene';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Config from '../misc/config.js';
import {QuickSettingsMenu} from './quickSettings.js';
import * as Main from './main.js';
import * as Util from '../misc/util.js';

import * as RemoteAccessStatus from './status/remoteAccess.js';
import * as PowerProfileStatus from './status/powerProfiles.js';
import * as RFKillStatus from './status/rfkill.js';
import * as CameraStatus from './status/camera.js';
import * as VolumeStatus from './status/volume.js';
import * as BrightnessStatus from './status/brightness.js';
import * as SystemStatus from './status/system.js';
import * as LocationStatus from './status/location.js';
import * as NightLightStatus from './status/nightLight.js';
import * as DarkModeStatus from './status/darkMode.js';
import * as DoNotDisturb from './status/doNotDisturb.js';
import * as BacklightStatus from './status/backlight.js';
import * as ThunderboltStatus from './status/thunderbolt.js';
import * as AutoRotateStatus from './status/autoRotate.js';
import * as BackgroundAppsStatus from './status/backgroundApps.js';

import {DateMenuButton} from './dateMenu.js';
import {ATIndicator} from './status/accessibility.js';
import {InputSourceIndicator} from './status/keyboard.js';
import {DwellClickIndicator} from './status/dwellClick.js';
import {ScreenRecordingIndicator, ScreenSharingIndicator} from './status/remoteAccess.js';

const BUTTON_DND_ACTIVATION_TIMEOUT = 250;

const N_QUICK_SETTINGS_COLUMNS = 2;

const INACTIVE_WORKSPACE_DOT_SCALE = 0.75;

const WorkspaceDot = GObject.registerClass({
    Properties: {
        'expansion': GObject.ParamSpec.double('expansion', null, null,
            GObject.ParamFlags.READWRITE,
            0.0, 1.0, 0.0),
        'width-multiplier': GObject.ParamSpec.double(
            'width-multiplier', null, null,
            GObject.ParamFlags.READWRITE,
            1.0, 10.0, 1.0),
    },
}, class WorkspaceDot extends Clutter.Actor {
    constructor(params = {}) {
        super({
            pivot_point: new Graphene.Point({x: 0.5, y: 0.5}),
            ...params,
        });

        this._dot = new St.Widget({
            style_class: 'workspace-dot',
            y_align: Clutter.ActorAlign.CENTER,
            pivot_point: new Graphene.Point({x: 0.5, y: 0.5}),
            request_mode: Clutter.RequestMode.WIDTH_FOR_HEIGHT,
        });
        this.add_child(this._dot);

        this.connect('notify::width-multiplier', () => this.queue_relayout());
        this.connect('notify::expansion', () => {
            this._updateVisuals();
            this.queue_relayout();
        });
        this._updateVisuals();

        this._destroying = false;
    }

    _updateVisuals() {
        const {expansion} = this;

        this._dot.set({
            opacity: Util.lerp(0.50, 1.0, expansion) * 255,
            scaleX: Util.lerp(INACTIVE_WORKSPACE_DOT_SCALE, 1.0, expansion),
            scaleY: Util.lerp(INACTIVE_WORKSPACE_DOT_SCALE, 1.0, expansion),
        });
    }

    vfunc_get_preferred_width(forHeight) {
        const factor = Util.lerp(1.0, this.widthMultiplier, this.expansion);
        return this._dot.get_preferred_width(forHeight).map(v => Math.round(v * factor));
    }

    vfunc_get_preferred_height(forWidth) {
        return this._dot.get_preferred_height(forWidth);
    }

    vfunc_allocate(box) {
        this.set_allocation(box);

        box.set_origin(0, 0);
        this._dot.allocate(box);
    }

    scaleIn() {
        this.set({
            scale_x: 0,
            scale_y: 0,
        });

        this.ease({
            duration: 500,
            mode: Clutter.AnimationMode.EASE_OUT_CUBIC,
            scale_x: 1.0,
            scale_y: 1.0,
        });
    }

    scaleOutAndDestroy() {
        this._destroying = true;

        this.ease({
            duration: 500,
            mode: Clutter.AnimationMode.EASE_OUT_CUBIC,
            scale_x: 0.0,
            scale_y: 0.0,
            onComplete: () => this.destroy(),
        });
    }

    get destroying() {
        return this._destroying;
    }
});

const WorkspaceIndicators = GObject.registerClass(
class WorkspaceIndicators extends St.BoxLayout {
    constructor() {
        super();

        this._workspacesAdjustment = Main.createWorkspacesAdjustment(this);
        this._workspacesAdjustment.connectObject(
            'notify::value', () => this._updateExpansion(),
            'notify::upper', () => this._recalculateDots(),
            this);

        for (let i = 0; i < this._workspacesAdjustment.upper; i++)
            this.insert_child_at_index(new WorkspaceDot(), i);
        this._updateExpansion();
    }

    _getActiveIndicators() {
        return [...this].filter(i => !i.destroying);
    }

    _recalculateDots() {
        const activeIndicators = this._getActiveIndicators();
        const nIndicators = activeIndicators.length;
        const targetIndicators = this._workspacesAdjustment.upper;

        let remaining = Math.abs(nIndicators - targetIndicators);
        while (remaining--) {
            if (nIndicators < targetIndicators) {
                const indicator = new WorkspaceDot();
                this.add_child(indicator);
                indicator.scaleIn();
            } else {
                const indicator = activeIndicators[nIndicators - remaining - 1];
                indicator.scaleOutAndDestroy();
            }
        }

        this._updateExpansion();
    }

    _updateExpansion() {
        const nIndicators = this._getActiveIndicators().length;
        const activeWorkspace = this._workspacesAdjustment.value;

        let widthMultiplier;
        if (nIndicators <= 2)
            widthMultiplier = 3.625;
        else if (nIndicators <= 5)
            widthMultiplier = 3.25;
        else
            widthMultiplier = 2.75;

        this.get_children().forEach((indicator, index) => {
            const distance = Math.abs(index - activeWorkspace);
            indicator.expansion = Math.clamp(1 - distance, 0, 1);
            indicator.widthMultiplier = widthMultiplier;
        });
    }
});

class ActivitiesButton extends Shell.PanelMenuButton {
    static [GObject.interfaces] = [Shell.DragTarget];

    static {
        GObject.registerClass(this);

        const bindingPool = this.get_binding_pool();

        bindingPool.install_closure(
            'toggle', Clutter.KEY_Return, Clutter.RELEASE_MASK,
            obj => {
                obj._toggleAction();
                return Clutter.EVENT_STOP;
            });
        bindingPool.install_closure(
            'toggle', Clutter.KEY_space, Clutter.RELEASE_MASK,
            obj => {
                obj._toggleAction();
                return Clutter.EVENT_STOP;
            });
    }

    _init() {
        super._init({
            menu_alignment: 0.0,
            dont_create_menu: true,
        });

        this.set({
            name: 'panelActivities',
            accessible_role: Atk.Role.TOGGLE_BUTTON,
            /* Translators: If there is no suitable word for "Activities"
               in your language, you can use the word for "Overview". */
            accessible_name: _('Activities'),
        });

        this.add_child(new WorkspaceIndicators());

        Main.overview.connectObject('showing',
            () => this.add_style_pseudo_class('checked'),
            this);
        Main.overview.connectObject('hiding',
            () => this.remove_style_pseudo_class('checked'),
            this);

        this._xdndTimeOut = 0;

        this._clickGesture = new Clutter.ClickGesture();
        this._clickGesture.connect('recognize', () => {
            if (Main.overview.shouldToggleByCornerOrButton())
                Main.overview.toggle();
        });
        this.add_action(this._clickGesture);
    }

    vfunc_handle_drag_over(source, _actor, _x, _y, _time) {
        if (source !== Main.xdndHandler)
            return Shell.DragMotionResult.CONTINUE;

        if (this._xdndTimeOut !== 0)
            GLib.source_remove(this._xdndTimeOut);
        this._xdndTimeOut = GLib.timeout_add_once(GLib.PRIORITY_DEFAULT, BUTTON_DND_ACTIVATION_TIMEOUT, () => {
            this._xdndToggleOverview();
        });
        GLib.Source.set_name_by_id(this._xdndTimeOut, '[gnome-shell] this._xdndToggleOverview');

        return Shell.DragMotionResult.CONTINUE;
    }

    vfunc_scroll_event(event) {
        return Main.wm.handleWorkspaceScroll(event);
    }

    _toggleAction() {
        if (Main.overview.shouldToggleByCornerOrButton()) {
            Main.overview.toggle();
            return Clutter.EVENT_STOP;
        }

        return Clutter.EVENT_PROPAGATE;
    }

    _xdndToggleOverview() {
        const [x, y] = global.get_pointer();
        const pickedActor = global.stage.get_actor_at_pos(Clutter.PickMode.REACTIVE, x, y);

        if (pickedActor === this && Main.overview.shouldToggleByCornerOrButton())
            Main.overview.toggle();

        GLib.source_remove(this._xdndTimeOut);
        this._xdndTimeOut = 0;
    }
}

const UnsafeModeIndicator = GObject.registerClass(
class UnsafeModeIndicator extends Shell.SystemIndicator {
    _init() {
        super._init();

        this._indicator = this.add_indicator();
        this._indicator.icon_name = 'channel-insecure-symbolic';

        global.context.bind_property('unsafe-mode',
            this._indicator, 'visible',
            GObject.BindingFlags.SYNC_CREATE);
    }
});

const QuickSettings = GObject.registerClass(
class QuickSettings extends Shell.PanelMenuButton {
    constructor() {
        super({
            menu_alignment: 0.0,
            accessible_name: C_('System menu in the top bar', 'System'),
            dont_create_menu: true,
        });

        this._indicators = new St.BoxLayout({
            style_class: 'panel-status-indicators-box',
        });
        this.add_child(this._indicators);

        this.set_menu(new QuickSettingsMenu(this, N_QUICK_SETTINGS_COLUMNS));

        this._setupIndicators().catch(error =>
            logError(error, 'Failed to setup quick settings'));
    }

    async _setupIndicators() {
        if (Config.HAVE_NETWORKMANAGER) {
            /** @type {import('./status/network.js')} */
            const NetworkStatus = await import('./status/network.js');

            this._network = new NetworkStatus.Indicator();
        } else {
            this._network = null;
        }

        if (Config.HAVE_BLUETOOTH) {
            /** @type {import('./status/bluetooth.js')} */
            const BluetoothStatus = await import('./status/bluetooth.js');

            this._bluetooth = new BluetoothStatus.Indicator();
        } else {
            this._bluetooth = null;
        }

        this._system = new SystemStatus.Indicator();
        this._camera = new CameraStatus.Indicator();
        this._volumeOutput = new VolumeStatus.OutputIndicator();
        this._volumeInput = new VolumeStatus.InputIndicator();
        this._brightness = new BrightnessStatus.Indicator();
        this._remoteAccess = new RemoteAccessStatus.RemoteAccessApplet();
        this._location = new LocationStatus.Indicator();
        this._thunderbolt = new ThunderboltStatus.Indicator();
        this._nightLight = new NightLightStatus.Indicator();
        this._darkMode = new DarkModeStatus.Indicator();
        this._doNotDisturb = new DoNotDisturb.Indicator();
        this._backlight = new BacklightStatus.Indicator();
        this._powerProfiles = new PowerProfileStatus.Indicator();
        this._rfkill = new RFKillStatus.Indicator();
        this._autoRotate = new AutoRotateStatus.Indicator();
        this._unsafeMode = new UnsafeModeIndicator();
        this._backgroundApps = new BackgroundAppsStatus.Indicator();

        // add privacy-related indicators before any external indicators
        let pos = 0;
        this._indicators.insert_child_at_index(this._remoteAccess, pos++);
        this._indicators.insert_child_at_index(this._camera, pos++);
        this._indicators.insert_child_at_index(this._volumeInput, pos++);
        this._indicators.insert_child_at_index(this._location, pos++);

        // append all other indicators
        this._indicators.add_child(this._brightness);
        this._indicators.add_child(this._thunderbolt);
        this._indicators.add_child(this._nightLight);
        if (this._network)
            this._indicators.add_child(this._network);
        this._indicators.add_child(this._darkMode);
        this._indicators.add_child(this._doNotDisturb);
        this._indicators.add_child(this._backlight);
        if (this._bluetooth)
            this._indicators.add_child(this._bluetooth);
        this._indicators.add_child(this._rfkill);
        this._indicators.add_child(this._autoRotate);
        this._indicators.add_child(this._volumeOutput);
        this._indicators.add_child(this._unsafeMode);
        this._indicators.add_child(this._powerProfiles);
        this._indicators.add_child(this._system);

        // add our quick settings items before any external ones
        const sibling = this.menu.getFirstItem();
        this._addItemsBefore(this._system.get_quick_settings_items(),
            sibling, N_QUICK_SETTINGS_COLUMNS);
        this._addItemsBefore(this._volumeOutput.get_quick_settings_items(),
            sibling, N_QUICK_SETTINGS_COLUMNS);
        this._addItemsBefore(this._volumeInput.get_quick_settings_items(),
            sibling, N_QUICK_SETTINGS_COLUMNS);
        this._addItemsBefore(this._brightness.get_quick_settings_items(),
            sibling, N_QUICK_SETTINGS_COLUMNS);

        this._addItemsBefore(this._camera.get_quick_settings_items(), sibling);
        this._addItemsBefore(this._remoteAccess.get_quick_settings_items(), sibling);
        this._addItemsBefore(this._thunderbolt.get_quick_settings_items(), sibling);
        this._addItemsBefore(this._location.get_quick_settings_items(), sibling);
        if (this._network)
            this._addItemsBefore(this._network.get_quick_settings_items(), sibling);
        if (this._bluetooth)
            this._addItemsBefore(this._bluetooth.get_quick_settings_items(), sibling);
        this._addItemsBefore(this._powerProfiles.get_quick_settings_items(), sibling);
        this._addItemsBefore(this._nightLight.get_quick_settings_items(), sibling);
        this._addItemsBefore(this._darkMode.get_quick_settings_items(), sibling);
        this._addItemsBefore(this._doNotDisturb.get_quick_settings_items(), sibling);
        this._addItemsBefore(this._backlight.get_quick_settings_items(), sibling);
        this._addItemsBefore(this._rfkill.get_quick_settings_items(), sibling);
        this._addItemsBefore(this._autoRotate.get_quick_settings_items(), sibling);
        this._addItemsBefore(this._unsafeMode.get_quick_settings_items(), sibling);

        // append background apps
        this._backgroundApps.get_quick_settings_items().forEach(
            item => this.menu.addItem(item, N_QUICK_SETTINGS_COLUMNS));
    }

    _addItemsBefore(items, sibling, colSpan = 1) {
        items.forEach(item => this.menu.insertItemBefore(item, sibling, colSpan));
    }

    /**
     * Insert indicator and quick settings items at
     * appropriate positions
     *
     * @param {Shell.SystemIndicator} indicator
     * @param {number=} colSpan
     */
    addExternalIndicator(indicator, colSpan = 1) {
        // Insert before first non-privacy indicator if it exists
        let sibling = this._brightness ?? null;
        this._indicators.insert_child_below(indicator, sibling);

        // Insert before background apps if it exists
        sibling = this._backgroundApps?.get_quick_settings_items().at(-1) ?? null;
        this._addItemsBefore(indicator.get_quick_settings_items(), sibling, colSpan);
    }
});

const PANEL_ITEM_IMPLEMENTATIONS = {
    'activities': ActivitiesButton,
    'quickSettings': QuickSettings,
    'dateMenu': DateMenuButton,
    'a11y': ATIndicator,
    'keyboard': InputSourceIndicator,
    'dwellClick': DwellClickIndicator,
    'screenRecording': ScreenRecordingIndicator,
    'screenSharing': ScreenSharingIndicator,
};

/**
 * Creates the indicator of the shell for a status area role. The panel (in
 * C) asks for it through Shell.Panel::create-indicator when the session mode
 * lists a role that has no indicator yet; main.js connects the signal to this
 * function.
 *
 * @param {string} role - the role, a key of PANEL_ITEM_IMPLEMENTATIONS
 * @returns {?Shell.PanelButtonBox} the new indicator, null if the role is not implemented
 */
export function createPanelItem(role) {
    // This icon is not implemented (this is a bug)
    if (!Object.hasOwn(PANEL_ITEM_IMPLEMENTATIONS, role))
        return null;

    const constructor = PANEL_ITEM_IMPLEMENTATIONS[role];
    return new constructor();
}
