import GdkPixbuf from 'gi://GdkPixbuf';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Shell from 'gi://Shell';

import * as Config from '../misc/config.js';
import * as Main from './main.js';

import {NotificationErrors, NotificationError} from '../misc/dbusErrors.js';

const FdoNotificationsIface = Shell.dbus_load_interface_xml('org.freedesktop.Notifications');

const NOTIFICATION_SERIALIZED_NAME = 'notifications';
const NOTIFICATION_SERIALIZED_FORMAT = 'a(sa(sv))';

/** @enum {number} */
const NotificationClosedReason = {
    EXPIRED: 1,
    DISMISSED: 2,
    APP_CLOSED: 3,
    UNDEFINED: 4,
};

/** @enum {number} */
const Urgency = {
    LOW: 0,
    NORMAL: 1,
    CRITICAL: 2,
};

class FdoNotificationDaemon {
    constructor() {
        this._dbusImpl = Gio.DBusExportedObject.wrapJSObject(FdoNotificationsIface, this);
        this._dbusImpl.export(Gio.DBus.session, '/org/freedesktop/Notifications');

        this._sourcesForApp = new Map();
        this._sourceForPidAndName = new Map();
        this._notifications = new Map();

        this._nextNotificationId = 1;
    }

    _imageForNotificationData(hints) {
        if (hints['image-data']) {
            const [
                width, height, rowStride, hasAlpha,
                bitsPerSample, nChannels_, data,
            ] = hints['image-data'];
            return Shell.util_create_pixbuf_from_data(data,
                GdkPixbuf.Colorspace.RGB,
                hasAlpha,
                bitsPerSample,
                width,
                height,
                rowStride);
        } else if (hints['image-path']) {
            return this._iconForNotificationData(hints['image-path']);
        }
        return null;
    }

    _iconForNotificationData(icon) {
        if (icon) {
            if (icon.startsWith('file://'))
                return new Gio.FileIcon({file: Gio.File.new_for_uri(icon)});
            else if (icon.startsWith('/'))
                return new Gio.FileIcon({file: Gio.File.new_for_path(icon)});
            else
                return new Gio.ThemedIcon({name: icon});
        }
        return null;
    }

    _getApp(pid, appId, appName) {
        const appSys = Shell.AppSystem.get_default();
        let app;

        app = Shell.WindowTracker.get_default().get_app_from_pid(pid);
        if (!app && appId)
            app = appSys.lookup_app(`${appId}.desktop`);

        if (!app)
            app = appSys.lookup_app(`${appName}.desktop`);

        return app;
    }

    // Returns the source associated with an app.
    //
    // If no existing source is found a new one is created.
    _getSourceForApp(sender, app) {
        let source = this._sourcesForApp.get(app);

        if (source)
            return source;

        source = new FdoNotificationDaemonSource(sender, app);

        if (app) {
            this._sourcesForApp.set(app, source);
            source.connect('destroy', () => {
                this._sourcesForApp.delete(app);
            });
        }

        Main.messageTray.add(source);
        return source;
    }

    // Returns the source associated with a pid and the app name.
    //
    // If no existing source is found, a new one is created.
    _getSourceForPidAndName(sender, pid, appName) {
        const key = `${pid}${appName}`;
        let source = this._sourceForPidAndName.get(key);

        if (source)
            return source;

        source = new FdoNotificationDaemonSource(sender, null);

        // Only check whether we have a PID since it's enough to identify
        // uniquely an app and "" is a valid app name.
        if (pid) {
            this._sourceForPidAndName.set(key, source);
            source.connect('destroy', () => {
                this._sourceForPidAndName.delete(key);
            });
        }

        Main.messageTray.add(source);
        return source;
    }

    NotifyAsync(params, invocation) {
        let [appName, replacesId, appIcon, summary, body, actions, hints, timeout_] = params;
        let id;

        for (const hint in hints) {
            // unpack the variants
            hints[hint] = hints[hint].deepUnpack();
        }

        hints = {urgency: Urgency.NORMAL, ...hints};

        // Be compatible with the various hints for image data and image path
        // 'image-data' and 'image-path' are the latest name of these hints, introduced in 1.2

        if (!hints['image-path'] && hints['image_path'])
            hints['image-path'] = hints['image_path']; // version 1.1 of the spec

        if (!hints['image-data']) {
            if (hints['image_data'])
                hints['image-data'] = hints['image_data']; // version 1.1 of the spec
            else if (hints['icon_data'] && !hints['image-path'])
                // early versions of the spec; 'icon_data' should only be used if 'image-path' is not available
                hints['image-data'] = hints['icon_data'];
        }

        let source, notification;
        if (replacesId !== 0 && this._notifications.has(replacesId)) {
            notification = this._notifications.get(replacesId);
            source = notification.source;
            id = replacesId;
        } else {
            const sender = hints['x-shell-sender'];
            const pid = hints['x-shell-sender-pid'];
            const appId = hints['desktop-entry'];
            const app = this._getApp(pid, appId, appName);

            id = this._nextNotificationId++;
            source = app
                ? this._getSourceForApp(sender, app)
                : this._getSourceForPidAndName(sender, pid, appName);

            notification = new Shell.Notification({source});
            this._notifications.set(id, notification);
            notification.connect('destroy', (n, reason) => {
                this._notifications.delete(id);
                let notificationClosedReason;
                switch (reason) {
                case Shell.NotificationDestroyedReason.EXPIRED:
                    notificationClosedReason = NotificationClosedReason.EXPIRED;
                    break;
                case Shell.NotificationDestroyedReason.DISMISSED:
                    notificationClosedReason = NotificationClosedReason.DISMISSED;
                    break;
                case Shell.NotificationDestroyedReason.SOURCE_CLOSED:
                    notificationClosedReason = NotificationClosedReason.APP_CLOSED;
                    break;
                default:
                    notificationClosedReason = NotificationClosedReason.UNDEFINED;
                    break;
                }
                this._emitNotificationClosed(id, notificationClosedReason, invocation);
                notification.disconnectObject(this);
            });
        }

        const gicon = this._imageForNotificationData(hints);

        const soundFile = 'sound-file' in hints
            ? Gio.File.new_for_path(hints['sound-file']) : null;

        notification.set({
            title: summary,
            body,
            gicon,
            use_body_markup: true,
            sound: new Shell.NotificationSound({
                file: soundFile,
                name: hints['sound-name'] ?? null,
            }),
            acknowledged: false,
        });
        notification.clear_actions();
        notification.disconnectObject(this);

        let hasDefaultAction = false;

        if (actions.length) {
            for (let i = 0; i < actions.length - 1; i += 2) {
                const [actionId, label] = [actions[i], actions[i + 1]];
                if (actionId === 'default') {
                    hasDefaultAction = true;
                } else {
                    notification.add_action(label, () => {
                        this._emitActivationToken(source, id, invocation);
                        this._emitActionInvoked(id, actionId, invocation);
                    });
                }
            }
        }

        if (hasDefaultAction) {
            notification.connectObject('activated', () => {
                this._emitActivationToken(source, id, invocation);
                this._emitActionInvoked(id, 'default', invocation);
            }, this);
        } else {
            notification.connectObject('activated', () => {
                source.open();
            }, this);
        }

        switch (hints.urgency) {
        case Urgency.LOW:
            notification.urgency = Shell.NotificationUrgency.LOW;
            break;
        case Urgency.NORMAL:
            notification.urgency = Shell.NotificationUrgency.NORMAL;
            break;
        case Urgency.CRITICAL:
            notification.urgency = Shell.NotificationUrgency.CRITICAL;
            break;
        }
        notification.resident = !!hints.resident;
        // 'transient' is a reserved keyword in JS, so we have to retrieve the value
        // of the 'transient' hint with hints['transient'] rather than hints.transient
        notification.is_transient = !!hints['transient'];

        const privacyScope = hints['x-gnome-privacy-scope'] || 'user';
        notification.privacy_scope = privacyScope === 'system'
            ? Shell.NotificationPrivacyScope.SYSTEM
            : Shell.NotificationPrivacyScope.USER;

        // Only fallback to 'app-icon' when the source doesn't have a valid app
        const sourceGIcon = source.app ? null : this._iconForNotificationData(appIcon);
        source.processNotification(notification, appName, sourceGIcon);

        return invocation.return_value(GLib.Variant.new('(u)', [id]));
    }

    CloseNotification(id) {
        const notification = this._notifications.get(id);
        notification?.destroy(Shell.NotificationDestroyedReason.SOURCE_CLOSED);
    }

    GetCapabilities() {
        return [
            'actions',
            // 'action-icons',
            'body',
            // 'body-hyperlinks',
            // 'body-images',
            'body-markup',
            // 'icon-multi',
            'icon-static',
            'persistence',
            'sound',
        ];
    }

    GetServerInformation() {
        return [
            Config.PACKAGE_NAME,
            'GNOME',
            Config.PACKAGE_VERSION,
            '1.2',
        ];
    }

    _emitNotificationClosed(id, reason, invocation) {
        const sender = invocation.get_sender();
        Shell.dbus_emit_signal_to_destination(this._dbusImpl, sender, 'NotificationClosed',
            GLib.Variant.new('(uu)', [id, reason]));
    }

    _emitActionInvoked(id, action, invocation) {
        const sender = invocation.get_sender();
        Shell.dbus_emit_signal_to_destination(this._dbusImpl, sender, 'ActionInvoked',
            GLib.Variant.new('(us)', [id, action]));
    }

    _emitActivationToken(source, id, invocation) {
        const sender = invocation.get_sender();
        const context = global.create_app_launch_context(0, -1);
        const token = context.get_startup_notify_id(null, []);
        Shell.dbus_emit_signal_to_destination(this._dbusImpl, sender, 'ActivationToken',
            GLib.Variant.new('(us)', [id, token]));
    }
}

export const FdoNotificationDaemonSource = GObject.registerClass(
class FdoNotificationDaemonSource extends Shell.Source {
    _init(sender, app) {
        super._init({
            policy: Shell.NotificationPolicy.new_for_app(app),
            title: app?.get_name() ?? null,
            icon: app?.get_icon() ?? null,
        });

        this.app = app;
        this._appName = null;
        this._appIcon = null;

        if (sender) {
            this._nameWatcherId = Gio.DBus.session.watch_name(sender,
                Gio.BusNameWatcherFlags.NONE,
                null,
                this._onNameVanished.bind(this));
        } else {
            this._nameWatcherId = 0;
        }
    }

    _onNameVanished() {
        // Destroy the notification source when its sender is removed from DBus.
        // Only do so if this.app is set to avoid removing "notify-send" sources, senders
        // of which аre removed from DBus immediately.
        // Sender being removed from DBus would normally result in a tray icon being removed,
        // so allow the code path that handles the tray icon being removed to handle that case.
        if (this.app)
            this.destroy(Shell.NotificationDestroyedReason.DISMISSED);
    }

    processNotification(notification, appName, appIcon) {
        if (!this.app && appName) {
            this._appName = appName;
            this.title = appName;
        }

        if (!this.app && appIcon) {
            this._appIcon = appIcon;
            this.icon = appIcon;
        }

        const tracker = Shell.WindowTracker.get_default();
        // Acknowledge notifications that are resident and their app has the
        // current focus so that we don't show a banner.
        if (notification.resident && this.app && tracker.focus_app === this.app)
            notification.acknowledged = true;

        this.add_notification(notification);
    }

    vfunc_open() {
        this.openApp();
        this.destroy_non_resident_notifications();
    }

    openApp() {
        if (this.app == null)
            return;

        this.app.activate();
        Main.overview.hide();
        Main.panel.close_calendar();
    }

    vfunc_destroy(reason) {
        if (this._nameWatcherId) {
            Gio.DBus.session.unwatch_name(this._nameWatcherId);
            this._nameWatcherId = 0;
        }

        super.vfunc_destroy(reason);
    }
});

const PRIORITY_URGENCY_MAP = {
    low: Shell.NotificationUrgency.LOW,
    normal: Shell.NotificationUrgency.NORMAL,
    high: Shell.NotificationUrgency.HIGH,
    urgent: Shell.NotificationUrgency.CRITICAL,
};

const GtkNotificationDaemonNotification = GObject.registerClass(
class GtkNotificationDaemonNotification extends Shell.Notification {
    _init(source, id, notification) {
        super._init({source});
        this._serialized = GLib.Variant.new('a{sv}', notification);
        this.id = id;

        const {
            title,
            body,
            icon: gicon,
            urgent,
            priority,
            buttons,
            'default-action': defaultAction,
            'default-action-target': defaultActionTarget,
            timestamp: time,
        } = notification;

        if (priority) {
            const urgency = PRIORITY_URGENCY_MAP[priority.unpack()];
            this.urgency = urgency !== undefined ? urgency : Shell.NotificationUrgency.NORMAL;
        } else if (urgent) {
            this.urgency = urgent.unpack()
                ? Shell.NotificationUrgency.CRITICAL
                : Shell.NotificationUrgency.NORMAL;
        } else {
            this.urgency = Shell.NotificationUrgency.NORMAL;
        }

        if (buttons) {
            buttons.deepUnpack().forEach(button => {
                this.add_action(button.label.unpack(), () => {
                    this._onButtonClicked(button);
                });
            });
        }

        this._defaultAction = defaultAction?.unpack();
        this._defaultActionTarget = defaultActionTarget;

        this.set({
            title: title.unpack(),
            body: body?.unpack(),
            gicon: gicon
                ? Gio.icon_deserialize(gicon) : null,
            datetime: time
                ? GLib.DateTime.new_from_unix_local(time.unpack()) : null,
        });
    }

    _activateAction(actionId, target) {
        if (actionId.startsWith('app.'))
            this.source.activateAction(actionId.slice('app.'.length), target);
        else
            this.source.emitActionInvoked(this.id, actionId, target);

        Main.overview.hide();
        Main.panel.close_calendar();
    }

    _onButtonClicked(button) {
        const {action, target} = button;
        this._activateAction(action.unpack(), target);
    }

    vfunc_activate() {
        if (this._defaultAction)
            this._activateAction(this._defaultAction, this._defaultActionTarget);
        else
            this.source.open();

        super.vfunc_activate();
    }

    serialize() {
        return this._serialized;
    }
});

function InvalidAppError() {}

export const GtkNotificationDaemonAppSource = GObject.registerClass(
class GtkNotificationDaemonAppSource extends Shell.Source {
    _init(appId, dbusImpl) {
        if (!Gio.Application.id_is_valid(appId))
            throw new InvalidAppError();

        const app = Shell.AppSystem.get_default().lookup_app(`${appId}.desktop`);
        if (!app)
            throw new InvalidAppError();

        super._init({
            title: app.get_name(),
            icon: app.get_icon(),
            policy: new Shell.NotificationApplicationPolicy({id: appId}),
        });

        this._appId = appId;
        this._app = app;
        this._dbusImpl = dbusImpl;

        this._notifications = {};
        this._notificationPending = false;
    }

    activateAction(actionId, target) {
        const params = target ? GLib.Variant.new('av', [target]) : null;
        this._app.activate_action(actionId, params, 0, -1, null).catch(error => {
            logError(error, `Failed to activate action for ${this._appId}`);
        });
    }

    emitActionInvoked(notificationId, actionId, target) {
        const context = global.create_app_launch_context(0, -1);
        const info = this._app.get_app_info();
        const token = context.get_startup_notify_id(info, []);

        this._dbusImpl.emit_signal('ActionInvoked',
            GLib.Variant.new('(sssava{sv})', [
                this._appId,
                notificationId,
                actionId,
                target ? [target] : [],
                {'activation-token': GLib.Variant.new_string(token)},
            ])
        );
    }

    vfunc_open() {
        this._app.activate();
        Main.overview.hide();
        Main.panel.close_calendar();
    }

    add_notification(notification) {
        this._notificationPending = true;

        this._notifications[notification.id]?.destroy(
            Shell.NotificationDestroyedReason.REPLACED);

        notification.connect('destroy', () => {
            delete this._notifications[notification.id];
        });
        this._notifications[notification.id] = notification;

        super.add_notification(notification);

        this._notificationPending = false;
    }

    vfunc_destroy(reason) {
        if (this._notificationPending)
            return;
        super.vfunc_destroy(reason);
    }

    removeNotification(notificationId) {
        if (this._notifications[notificationId])
            this._notifications[notificationId].destroy(Shell.NotificationDestroyedReason.SOURCE_CLOSED);
    }

    serialize() {
        const notifications = [];
        for (const notificationId in this._notifications) {
            const notification = this._notifications[notificationId];
            notifications.push([notificationId, notification.serialize()]);
        }
        return [this._appId, notifications];
    }
});

const GtkNotificationsIface = Shell.dbus_load_interface_xml('org.gtk.Notifications');

class GtkNotificationDaemon {
    constructor() {
        this._sources = {};

        this._dbusImpl = Gio.DBusExportedObject.wrapJSObject(GtkNotificationsIface, this);
        this._dbusImpl.export(Gio.DBus.session, '/org/gtk/Notifications');

        this._loadNotifications();

        Gio.DBus.session.own_name('org.gtk.Notifications', Gio.BusNameOwnerFlags.REPLACE, null, null);
    }

    _ensureAppSource(appId) {
        if (this._sources[appId])
            return this._sources[appId];

        const source = new GtkNotificationDaemonAppSource(appId, this._dbusImpl);

        source.connect('destroy', () => {
            delete this._sources[appId];
            this._saveNotifications();
        });
        source.connect('notify::count', this._saveNotifications.bind(this));
        Main.messageTray.add(source);
        this._sources[appId] = source;
        return source;
    }

    _loadNotifications() {
        this._isLoading = true;

        try {
            const value = global.get_persistent_state(
                NOTIFICATION_SERIALIZED_FORMAT,
                NOTIFICATION_SERIALIZED_NAME);

            if (value) {
                const sources = value.deepUnpack();
                sources.forEach(([appId, notifications]) => {
                    if (notifications.length === 0)
                        return;

                    let source;
                    try {
                        source = this._ensureAppSource(appId);
                    } catch (e) {
                        if (e instanceof InvalidAppError)
                            return;
                        throw e;
                    }

                    notifications.forEach(([notificationId, notificationPacked]) => {
                        const notification = new GtkNotificationDaemonNotification(source,
                            notificationId,
                            notificationPacked.deepUnpack());
                        // Acknowledge all stored notification so that we don't show a banner again
                        notification.acknowledged = true;
                        source.add_notification(notification);
                    });
                });
            }
        } catch (e) {
            logError(e, 'Failed to load saved notifications');
        } finally {
            this._isLoading = false;
        }
    }

    _saveNotifications() {
        if (this._isLoading)
            return;

        const sources = [];
        for (const appId in this._sources) {
            const source = this._sources[appId];
            sources.push(source.serialize());
        }

        global.set_persistent_state(NOTIFICATION_SERIALIZED_NAME,
            sources.length
                ? new GLib.Variant(NOTIFICATION_SERIALIZED_FORMAT, sources)
                : null);
    }

    AddNotificationAsync(params, invocation) {
        const [appId, notificationId, notificationSerialized] = params;

        let source;
        try {
            source = this._ensureAppSource(appId);
        } catch (e) {
            if (e instanceof InvalidAppError) {
                invocation.return_error_literal(NotificationErrors,
                    NotificationError.INVALID_APP,
                    `The app by ID "${appId}" could not be found`);
                return;
            }
            throw e;
        }

        const timestamp = GLib.DateTime.new_now_local().to_unix();
        notificationSerialized['timestamp'] = new GLib.Variant('x', timestamp);

        const notification = new GtkNotificationDaemonNotification(source,
            notificationId,
            notificationSerialized);
        source.add_notification(notification);

        invocation.return_value(null);
    }

    RemoveNotificationAsync(params, invocation) {
        const [appId, notificationId] = params;
        const source = this._sources[appId];
        if (source)
            source.removeNotification(notificationId);

        invocation.return_value(null);
    }
}

export class NotificationDaemon {
    constructor() {
        this._fdoNotificationDaemon = new FdoNotificationDaemon();
        this._gtkNotificationDaemon = new GtkNotificationDaemon();
    }
}
