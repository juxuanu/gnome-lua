import Clutter from 'gi://Clutter';
import GObject from 'gi://GObject';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from './main.js';

export class BackgroundMenu extends Shell.PopupMenu {
    static {
        GObject.registerClass(this);
    }

    _init(layoutManager) {
        super._init({
            source_actor: layoutManager.dummy_cursor,
            arrow_alignment: 0,
            arrow_side: St.Side.TOP,
        });

        this.add_settings_action(_('Change Background…'), 'gnome-background-panel.desktop');
        this.add_menu_item(new Shell.PopupSeparatorMenuItem());
        this.add_settings_action(_('Display Settings'), 'gnome-display-panel.desktop');
        this.add_settings_action(_('Settings'), 'org.gnome.Settings.desktop');

        this.actor.add_style_class_name('background-menu');

        layoutManager.ui_group.add_child(this.actor);
        this.actor.hide();
    }
}

/**
 * @param {Meta.BackgroundActor} actor
 * @param {Shell.LayoutManager} layoutManager
 */
export function addBackgroundMenu(actor, layoutManager) {
    actor.reactive = true;
    actor._backgroundMenu = new BackgroundMenu(layoutManager);
    actor._backgroundManager = new Shell.PopupMenuManager();
    actor._backgroundManager.add_menu(actor._backgroundMenu);

    function openMenu(x, y) {
        Main.layoutManager.set_dummy_cursor_geometry(x, y, 0, 0);
        actor._backgroundMenu.open(Shell.PopupAnimation.FULL);
    }

    const longPressGesture = new Clutter.LongPressGesture({
        required_button: Clutter.BUTTON_PRIMARY,
    });
    longPressGesture.connect('recognize', () => {
        if (actor._backgroundMenu.is_open)
            return;

        const {x, y} = longPressGesture.get_coords_abs();
        openMenu(x, y);
    });
    actor.add_action(longPressGesture);

    const clickGesture = new Clutter.ClickGesture({
        required_button: Clutter.BUTTON_SECONDARY,
        recognize_on_press: true,
    });
    clickGesture.connect('recognize', () => {
        const {x, y} = clickGesture.get_coords_abs();
        openMenu(x, y);
    });
    actor.add_action(clickGesture);

    actor.connect('destroy', () => {
        actor._backgroundMenu.destroy();
        actor._backgroundMenu = null;
        actor._backgroundManager = null;
    });
}
