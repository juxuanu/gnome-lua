import GObject from 'gi://GObject';
import Shell from 'gi://Shell';

import * as Main from './main.js';

export class WindowAttentionHandler {
    constructor() {
        this._tracker = Shell.WindowTracker.get_default();
        global.display.connectObject(
            'window-demands-attention', this._onWindowDemandsAttention.bind(this),
            'window-marked-urgent', this._onWindowDemandsAttention.bind(this),
            this);
    }

    _getTitleAndBanner(app, window) {
        const title = app.get_name();
        const banner = _('“%s” is ready').format(window.get_title());
        return [title, banner];
    }

    _onWindowDemandsAttention(display, window) {
        // We don't want to show the notification when the window is already focused,
        // because this is rather pointless.
        // Some apps (like GIMP) do things like setting the urgency hint on the
        // toolbar windows which would result into a notification even though GIMP itself is
        // focused.
        // We are just ignoring the hint on skip_taskbar windows for now.
        // (Which is the same behaviour as with metacity + panel)

        if (!window || window.has_focus() || window.is_skip_taskbar())
            return;

        const app = this._tracker.get_window_app(window);
        const source = new WindowAttentionSource(app, window);
        Main.messageTray.add(source);

        let [title, body] = this._getTitleAndBanner(app, window);

        const notification = new Shell.Notification({
            source,
            title,
            body,
            for_feedback: true,
        });
        notification.connect('activated', () => {
            source.open();
        });

        source.add_notification(notification);

        window.connectObject('notify::title', () => {
            [title, body] = this._getTitleAndBanner(app, window);
            notification.set({title, body});
        }, source);
    }
}

const WindowAttentionSource = GObject.registerClass(
class WindowAttentionSource extends Shell.Source {
    _init(app, window) {
        super._init({
            title: app.get_name(),
            icon: app.get_icon(),
            policy: Shell.NotificationPolicy.new_for_app(app),
        });

        this._window = window;
        this._window.connectObject(
            'notify::demands-attention', this._sync.bind(this),
            'notify::urgent', this._sync.bind(this),
            'focus', () => this.destroy(),
            'unmanaged', () => this.destroy(), this);
    }

    _sync() {
        if (this._window.demands_attention || this._window.urgent)
            return;
        this.destroy();
    }

    vfunc_destroy(reason) {
        this._window.disconnectObject(this);

        super.vfunc_destroy(reason);
    }

    vfunc_open() {
        Main.activateWindow(this._window);
    }
});
