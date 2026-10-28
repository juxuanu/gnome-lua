import Clutter from 'gi://Clutter';
import GObject from 'gi://GObject';
import Shell from 'gi://Shell';
import St from 'gi://St';

import {QuickSlider, SystemIndicator} from '../quickSettings.js';
import * as Main from '../main.js';
import {registerDestroyableType} from '../../misc/signalTracker.js';

// The scales emit 'destroy' when the brightness manager drops them
registerDestroyableType(Shell.BrightnessScale);

const BRIGHTNESS_NAME = _('Brightness');

class BrightnessSliderMenu extends Shell.PopupMenuSection {
    static {
        GObject.registerClass(this);
    }

    addSlider(scale) {
        const text = new Shell.PopupMenuItem({
            text: scale.name,
            reactive: false,
            activatable: false,
            active_on_hover: false,
        });
        this.add_menu_item(text);

        const slider = new Shell.Slider({value: 0});
        slider.accessible_name = scale.name;

        const sliderBin = new St.Bin({
            style_class: 'slider-bin',
            child: slider,
            reactive: true,
            can_focus: true,
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
        });

        const sliderMenuItem = new Shell.PopupBaseMenuItem({
            reactive: false,
            activatable: false,
            active_on_hover: false,
        });
        sliderMenuItem.add_child(sliderBin);
        this.add_menu_item(sliderMenuItem);

        return slider;
    }
}

const BrightnessItem = GObject.registerClass(
class BrightnessItem extends QuickSlider {
    _init() {
        super._init({
            iconName: 'display-brightness-symbolic',
            menuButtonAccessibleName: _('Open brightness menu'),
        });

        this.slider.accessible_name = BRIGHTNESS_NAME;

        this.menu.setHeader('display-brightness-symbolic', BRIGHTNESS_NAME);
        this._monitorBrightnessSection = new BrightnessSliderMenu();
        this.menu.add_menu_item(this._monitorBrightnessSection);

        this._manager = Main.brightnessManager;
        this._manager.connectObject('changed',
            this._sync.bind(this), this);
        this._sync();
    }

    _sync() {
        const globalScale = this._manager.global_scale;
        const scales = this._manager.get_scales();
        this.set({
            visible: !!globalScale,
            menuEnabled: scales.length > 1,
        });

        if (!this.visible)
            return;

        this._monitorBrightnessSection.remove_all();

        this._connectSlider(this.slider, globalScale);
        for (const scale of scales) {
            const slider = this._monitorBrightnessSection.addSlider(scale);
            this._connectSlider(slider, scale);
        }
    }

    _connectSlider(slider, scale) {
        slider.disconnectObject(scale);
        slider.connectObject('notify::value', () => {
            if (slider._blockBrightnessAdjust)
                return;
            scale.value = slider.value;
        }, scale);

        const changeBrightness = () => {
            slider._blockBrightnessAdjust = true;
            slider.value = scale.value;
            slider._blockBrightnessAdjust = false;
        };
        scale.connectObject('notify::value', changeBrightness, slider);
        changeBrightness();
    }
});

export const Indicator = GObject.registerClass(
class Indicator extends SystemIndicator {
    _init() {
        super._init();

        this.quickSettingsItems.push(new BrightnessItem());
    }
});
