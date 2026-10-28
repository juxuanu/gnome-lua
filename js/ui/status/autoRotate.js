import Gio from 'gi://Gio';
import GObject from 'gi://GObject';
import Shell from 'gi://Shell';

import {QuickToggle, SystemIndicator} from '../quickSettings.js';

const RotationToggle = GObject.registerClass(
class RotationToggle extends QuickToggle {
    _init() {
        this._systemActions = Shell.SystemActions.get_default();

        super._init({
            title: _('Auto Rotate'),
        });

        this._systemActions.bind_property('can-lock-orientation',
            this, 'visible',
            GObject.BindingFlags.DEFAULT |
            GObject.BindingFlags.SYNC_CREATE);
        this._systemActions.bind_property('orientation-lock-icon',
            this, 'icon-name',
            GObject.BindingFlags.DEFAULT |
            GObject.BindingFlags.SYNC_CREATE);

        this._settings = new Gio.Settings({
            schema_id: 'org.gnome.settings-daemon.peripherals.touchscreen',
        });
        this._settings.bind('orientation-lock',
            this, 'checked',
            Gio.SettingsBindFlags.INVERT_BOOLEAN);

        this.connect('clicked',
            () => this._systemActions.activate_lock_orientation());
    }
});

export const Indicator = GObject.registerClass(
class Indicator extends SystemIndicator {
    _init() {
        super._init();

        this.quickSettingsItems.push(new RotationToggle());
    }
});
