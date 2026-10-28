import Clutter from 'gi://Clutter';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as Main from './main.js';
import * as Screenshot from './screenshot.js';

export class WindowMenu extends Shell.PopupMenu {
    static {
        GObject.registerClass(this);
    }

    _init(window, sourceActor) {
        super._init({
            source_actor: sourceActor,
            arrow_alignment: 0,
            arrow_side: St.Side.TOP,
        });

        this.actor.add_style_class_name('window-menu');

        Main.layoutManager.uiGroup.add_child(this.actor);
        this.actor.hide();

        this._buildMenu(window);
    }

    _buildMenu(window) {
        const type = window.get_window_type();

        let item;

        // Translators: entry in the window right click menu.
        item = this.add_action(C_('window-menu', 'Take Screenshot'), async () => {
            try {
                const actor = window.get_compositor_private();
                const content = actor.paint_to_content(null);
                const texture = content.get_texture();

                await Screenshot.captureScreenshot(texture, null, 1, null);
            } catch (e) {
                logError(e, 'Error capturing screenshot');
            }
        });

        item = this.add_action(_('Hide'), () => {
            window.minimize();
        });
        if (!window.can_minimize())
            item.set_sensitive(false);

        if (window.is_maximized()) {
            item = this.add_action(_('Restore'), () => {
                window.unmaximize();
            });
        } else {
            item = this.add_action(_('Maximize'), () => {
                window.maximize();
            });
        }
        if (!window.can_maximize())
            item.set_sensitive(false);

        item = this.add_action(_('Move'), event => {
            const backend = global.stage.get_context().get_backend();
            const sprite = backend.get_sprite(global.stage, event) ||
                backend.get_pointer_sprite(global.stage);

            window.begin_grab_op(
                Meta.GrabOp.KEYBOARD_MOVING,
                sprite,
                event.get_time(),
                null);
        });
        if (!window.allows_move())
            item.set_sensitive(false);

        item = this.add_action(_('Resize'), event => {
            const backend = global.stage.get_context().get_backend();
            const sprite = backend.get_sprite(global.stage, event) ||
                backend.get_pointer_sprite(global.stage);

            window.begin_grab_op(
                Meta.GrabOp.KEYBOARD_RESIZING_UNKNOWN,
                sprite,
                event.get_time(),
                null);
        });
        if (!window.allows_resize())
            item.set_sensitive(false);

        item = this.add_action(_('Always on Top'), () => {
            if (window.is_above())
                window.unmake_above();
            else
                window.make_above();
        });
        if (window.is_above())
            item.set_ornament(Shell.Ornament.CHECK);
        if (window.is_maximized() ||
            type === Meta.WindowType.DOCK ||
            type === Meta.WindowType.DESKTOP ||
            type === Meta.WindowType.SPLASHSCREEN)
            item.set_sensitive(false);

        if (Main.sessionMode.has_workspaces &&
            (!Meta.prefs_get_workspaces_only_on_primary() ||
             window.is_on_primary_monitor())) {
            const isSticky = window.is_on_all_workspaces();

            item = this.add_action(_('Always on Visible Workspace'), () => {
                if (isSticky)
                    window.unstick();
                else
                    window.stick();
            });
            if (isSticky)
                item.set_ornament(Shell.Ornament.CHECK);
            if (window.is_always_on_all_workspaces())
                item.set_sensitive(false);

            if (!isSticky) {
                const workspace = window.get_workspace();
                if (workspace !== workspace.get_neighbor(Meta.MotionDirection.LEFT)) {
                    this.add_action(_('Move to Workspace Left'), () => {
                        const dir = Meta.MotionDirection.LEFT;
                        window.change_workspace(workspace.get_neighbor(dir));
                    });
                }
                if (workspace !== workspace.get_neighbor(Meta.MotionDirection.RIGHT)) {
                    this.add_action(_('Move to Workspace Right'), () => {
                        const dir = Meta.MotionDirection.RIGHT;
                        window.change_workspace(workspace.get_neighbor(dir));
                    });
                }
                if (workspace !== workspace.get_neighbor(Meta.MotionDirection.UP)) {
                    this.add_action(_('Move to Workspace Up'), () => {
                        const dir = Meta.MotionDirection.UP;
                        window.change_workspace(workspace.get_neighbor(dir));
                    });
                }
                if (workspace !== workspace.get_neighbor(Meta.MotionDirection.DOWN)) {
                    this.add_action(_('Move to Workspace Down'), () => {
                        const dir = Meta.MotionDirection.DOWN;
                        window.change_workspace(workspace.get_neighbor(dir));
                    });
                }
            }
        }

        const display = global.display;
        const nMonitors = display.get_n_monitors();
        const monitorIndex = window.get_monitor();
        if (nMonitors > 1 && monitorIndex >= 0) {
            this.add_menu_item(new Shell.PopupSeparatorMenuItem());

            let dir = Meta.DisplayDirection.UP;
            const upMonitorIndex =
                display.get_monitor_neighbor_index(monitorIndex, dir);
            if (upMonitorIndex !== -1) {
                this.add_action(_('Move to Monitor Up'), () => {
                    window.move_to_monitor(upMonitorIndex);
                });
            }

            dir = Meta.DisplayDirection.DOWN;
            const downMonitorIndex =
                display.get_monitor_neighbor_index(monitorIndex, dir);
            if (downMonitorIndex !== -1) {
                this.add_action(_('Move to Monitor Down'), () => {
                    window.move_to_monitor(downMonitorIndex);
                });
            }

            dir = Meta.DisplayDirection.LEFT;
            const leftMonitorIndex =
                display.get_monitor_neighbor_index(monitorIndex, dir);
            if (leftMonitorIndex !== -1) {
                this.add_action(_('Move to Monitor Left'), () => {
                    window.move_to_monitor(leftMonitorIndex);
                });
            }

            dir = Meta.DisplayDirection.RIGHT;
            const rightMonitorIndex =
                display.get_monitor_neighbor_index(monitorIndex, dir);
            if (rightMonitorIndex !== -1) {
                this.add_action(_('Move to Monitor Right'), () => {
                    window.move_to_monitor(rightMonitorIndex);
                });
            }
        }

        this.add_menu_item(new Shell.PopupSeparatorMenuItem());

        item = this.add_action(_('Close'), event => {
            window.delete(event.get_time());
        });
        if (!window.can_close())
            item.set_sensitive(false);
    }

    add_action(label, callback) {
        const item = super.add_action(label, callback);
        item.set_ornament(Shell.Ornament.NONE);
        return item;
    }
}

export class WindowMenuManager {
    constructor() {
        this._manager = new Shell.PopupMenuManager();

        this._sourceActor = new St.Widget({reactive: true, visible: false});

        this._clickGesture = new Clutter.ClickGesture();
        this._clickGesture.set_recognize_on_press(true);
        this._clickGesture.connect('recognize', () => {
            this._manager.active_menu.toggle();
        });
        this._sourceActor.add_action(this._clickGesture);
        Main.uiGroup.add_child(this._sourceActor);
    }

    showWindowMenuForWindow(window, type, rect) {
        if (!Main.sessionMode.has_wm_menus)
            return;

        if (type !== Meta.WindowMenuType.WM)
            throw new Error('Unsupported window menu type');
        const menu = new WindowMenu(window, this._sourceActor);

        this._manager.add_menu(menu);

        menu.connect('activate', () => {
            window.check_alive(global.get_current_time());
        });
        const destroyId = window.connect('unmanaged', () => {
            menu.close(Shell.PopupAnimation.NONE);
        });

        this._sourceActor.set_size(Math.max(1, rect.width), Math.max(1, rect.height));
        this._sourceActor.set_position(rect.x, rect.y);
        this._sourceActor.show();

        menu.open(Shell.PopupAnimation.FADE);
        menu.actor.navigate_focus(null, St.DirectionType.TAB_FORWARD, false);
        menu.connect('open-state-changed', (menu_, isOpen) => {
            if (isOpen)
                return;

            this._sourceActor.hide();
            menu.destroy();
            window.disconnect(destroyId);
        });
    }
}
