// Load all required dependencies with the correct versions
import '../misc/dependencies.js';

import {setConsoleLogDomain} from 'console';
import * as Gettext from 'gettext';

import Cairo from 'cairo';
import Clutter from 'gi://Clutter';
import Gdk from 'gi://Gdk';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import Meta from 'gi://Meta';
import Polkit from 'gi://Polkit';
import Shell from 'gi://Shell';
import St from 'gi://St';

import * as SignalTracker from '../misc/signalTracker.js';
import {adjustAnimationTime} from '../misc/animationUtils.js';
import {logErrorUnlessCancelled} from '../misc/errorUtils.js';

setConsoleLogDomain('GNOME Shell');

Gio._promisify(Gio.DataInputStream.prototype, 'fill_async');
Gio._promisify(Gio.DataInputStream.prototype, 'read_line_async');
Gio._promisify(Gio.DBus, 'get');
Gio._promisify(Gio.DBusConnection.prototype, 'call');
Gio._promisify(Gio.DBusProxy, 'new');
Gio._promisify(Gio.DBusProxy.prototype, 'init_async');
Gio._promisify(Gio.DBusProxy.prototype, 'call_with_unix_fd_list');
Gio._promisify(Gio.File.prototype, 'query_info_async');
Gio._promisify(Gio.File.prototype, 'read_async');
Gio._promisify(Polkit.Permission, 'new');
Gio._promisify(Shell.App.prototype, 'activate_action');
Gio._promisify(Shell.LoginManager.prototype, 'get_current_user_async');
Gio._promisify(Shell.LoginManager.prototype, 'get_current_session_async');
Gio._promisify(Shell.LoginManager.prototype, 'can_reboot_to_boot_loader_menu_async');
Gio._promisify(Shell.LoginManager.prototype, 'list_sessions_async');
Gio._promisify(Shell.LoginManager.prototype, 'inhibit_async');
Gio._promisify(Shell.Login1Session.prototype, 'call_set_locked_hint');
Gio._promisify(Shell.SessionManager.prototype, 'call_can_shutdown');
Gio._promisify(Shell.SessionManager.prototype, 'call_can_reboot');
Gio._promisify(Shell.SessionManager.prototype, 'call_can_suspend');
Gio._promisify(Shell.SessionManager.prototype, 'call_is_inhibited');
Gio._promisify(Shell.SessionManager.prototype, 'call_logout');
Gio._promisify(Shell.SessionManager.prototype, 'call_shutdown');
Gio._promisify(Shell.SessionManager.prototype, 'call_reboot');
Gio._promisify(Shell.SessionManager.prototype, 'call_suspend');
Gio._promisify(Meta.Backend.prototype, 'set_keymap_async');

// We can't import shell JS modules yet, because they may have
// variable initializations, etc, that depend on this file's
// changes

function _patchLayoutClass(layoutClass, styleProps) {
    if (styleProps) {
        layoutClass.prototype.hookup_style = function (container) {
            container.connect('style-changed', () => {
                const node = container.get_theme_node();
                for (const prop in styleProps) {
                    const [found, length] = node.lookup_length(styleProps[prop], false);
                    if (found)
                        this[prop] = length;
                }
            });
        };
    }
}

function _makeEaseCallback(onComplete, onStopped) {
    const {promise, resolve, reject} = Promise.withResolvers();
    const callback = isFinished => {
        if (onStopped)
            onStopped(isFinished);
        if (onComplete && isFinished)
            onComplete();

        if (isFinished) {
            resolve();
        } else {
            reject(new GLib.Error(Gio.IOErrorEnum,
                Gio.IOErrorEnum.CANCELLED, 'Transition was stopped before completing'));
        }
    };

    return {promise, callback};
}

function _makeEaseParams({
    duration, delay, mode, repeatCount, autoReverse, animationRequired,
}) {
    // Every field is set, so we don't depend on how GJS allocates the boxed
    const easeParams = new Shell.EaseParams();
    easeParams.duration = duration ?? -1;
    easeParams.delay = delay === undefined ? -1 : delay;
    easeParams.mode = mode ?? 0;
    easeParams.repeat_count = repeatCount;
    easeParams.auto_reverse = autoReverse;
    easeParams.animation_required = animationRequired;
    return easeParams;
}

function _easeActor(actor, params) {
    const {
        onComplete, onStopped,
        duration, delay, mode,
        repeatCount = 0, autoReverse = false, animationRequired = false,
        ...props
    } = {...params};

    const easeParams = _makeEaseParams({
        duration, delay, mode, repeatCount, autoReverse, animationRequired,
    });
    const {promise, callback} = _makeEaseCallback(onComplete, onStopped);

    const animatedProps = Object.keys(props).map(p => p.replaceAll('_', '-'));

    // saves the easing state, cancels overwritten transitions
    if (Shell.actor_ease_begin(actor, easeParams, animatedProps))
        actor.set(props);
    // restores the easing state, then sets up the repeat and the completion
    Shell.actor_ease_end(actor, easeParams, animatedProps, callback);

    return promise;
}

function _easeAnimatableProperty(animatable, propName, target, params) {
    const {
        onComplete, onStopped,
        duration, delay, mode, progress_mode: progressMode,
        repeatCount = 0, autoReverse = false, animationRequired = false,
        ...unknown
    } = {...params};

    if (Object.keys(unknown).length > 0) {
        throw new Error(
            `Unsupported ease_property() parameters: ${Object.keys(unknown).join(', ')}`);
    }

    const easeParams = _makeEaseParams({
        duration,
        delay,
        // Avoid pointless difference with ease()
        mode: mode || progressMode,
        repeatCount,
        autoReverse,
        animationRequired,
    });
    const {promise, callback} = _makeEaseCallback(onComplete, onStopped);

    Shell.animatable_ease_property(animatable, propName, target, easeParams, callback);

    return promise;
}

// Add some bindings to the global JS namespace
globalThis.global = Shell.Global.get();

globalThis._ = Gettext.gettext;
globalThis.C_ = Gettext.pgettext;
globalThis.ngettext = Gettext.ngettext;
globalThis.N_ = s => s;

GObject.gtypeNameBasedOnJSPath = true;

GObject.Object.prototype.connectObject = function (...args) {
    SignalTracker.connectObject(this, ...args);
};
GObject.Object.prototype.connect_object = function (...args) {
    SignalTracker.connectObject(this, ...args);
};
GObject.Object.prototype.disconnectObject = function (...args) {
    SignalTracker.disconnectObject(this, ...args);
};
GObject.Object.prototype.disconnect_object = function (...args) {
    SignalTracker.disconnectObject(this, ...args);
};

SignalTracker.registerDestroyableType(Clutter.Actor);

Cairo.Context.prototype.setSourceColor = function (color) {
    const {red, green, blue, alpha} = color;
    const rgb = [red, green, blue].map(v => v / 255.0);

    if (alpha !== 0xff)
        this.setSourceRGBA(...rgb, alpha / 255.0);
    else
        this.setSourceRGB(...rgb);
};

// Miscellaneous monkeypatching
_patchLayoutClass(Clutter.GridLayout, {
    row_spacing: 'spacing-rows',
    column_spacing: 'spacing-columns',
});
_patchLayoutClass(Clutter.BoxLayout, {spacing: 'spacing'});

const origSetEasingDuration = Clutter.Actor.prototype.set_easing_duration;
Clutter.Actor.prototype.set_easing_duration = function (msecs, params = {}) {
    origSetEasingDuration.call(this, adjustAnimationTime(msecs, params));
};
const origSetEasingDelay = Clutter.Actor.prototype.set_easing_delay;
Clutter.Actor.prototype.set_easing_delay = function (msecs, params = {}) {
    origSetEasingDelay.call(this, adjustAnimationTime(msecs, params));
};

Clutter.Actor.prototype.ease = function (props) {
    _easeActor(this, props).catch(logErrorUnlessCancelled);
};
Clutter.Actor.prototype.ease_property = function (propName, target, params) {
    _easeAnimatableProperty(this, propName, target, params).catch(logErrorUnlessCancelled);
};
St.Adjustment.prototype.ease = function (target, params) {
    // we're not an actor of course, but we implement the same
    // transition API as Clutter.Actor, so this works anyway
    _easeAnimatableProperty(this, 'value', target, params).catch(logErrorUnlessCancelled);
};

Clutter.Actor.prototype.easeAsync = async function (props) {
    await _easeActor(this, props);
};
Clutter.Actor.prototype.ease_property_async = async function (propName, target, params) {
    await _easeAnimatableProperty(this, propName, target, params);
};
St.Adjustment.prototype.easeAsync = async function (target, params) {
    // we're not an actor of course, but we implement the same
    // transition API as Clutter.Actor, so this works anyway
    await _easeAnimatableProperty(this, 'value', target, params).catch(() => {});
};

Clutter.Actor.prototype[Symbol.iterator] = function* () {
    for (let c = this.get_first_child(); c; c = c.get_next_sibling())
        yield c;
};

Clutter.Actor.prototype.toString = function () {
    return St.describe_actor(this);
};

Gio.File.prototype.touch_async = function (callback) {
    Shell.util_touch_file_async(this, callback);
};
Gio.File.prototype.touch_finish = function (result) {
    return Shell.util_touch_file_finish(this, result);
};

const origToString = Object.prototype.toString;
Object.prototype.toString = function () {
    const base = origToString.call(this);
    try {
        if ('actor' in this && this.actor instanceof Clutter.Actor)
            return base.replace(/\]$/, ` delegate for ${this.actor.toString().substring(1)}`);
        else
            return base;
    } catch {
        return base;
    }
};

function defineCompatButtonMaskProp(oldName, newName) {
    const prop = {};

    if (Object.hasOwn(St.ButtonMask, oldName))
        return prop;

    prop[oldName] = {
        get: () => {
            console.warn(`St.ButtonMask.${oldName} is deprecated, ` +
                `use St.ButtonMask.${newName} instead.`);
            return St.ButtonMask[newName];
        },
    };

    return prop;
}
Object.defineProperties(St.ButtonMask, {
    ...defineCompatButtonMaskProp('ONE', 'PRIMARY'),
    ...defineCompatButtonMaskProp('TWO', 'MIDDLE'),
    ...defineCompatButtonMaskProp('THREE', 'SECONDARY'),
});

const slowdownEnv = GLib.getenv('GNOME_SHELL_SLOWDOWN_FACTOR');
if (slowdownEnv) {
    const factor = parseFloat(slowdownEnv);
    if (!isNaN(factor) && factor > 0.0)
        St.Settings.get().slow_down_factor = factor;
}

function wrapSpawnFunction(func) {
    const originalFunc = GLib[func];
    return function (workingDirectory, argv, envp, flags, childSetup, ...args) {
        const commonArgs = [workingDirectory, argv, envp, flags];
        if (childSetup) {
            logError(new Error(`Using child GLib.${func} with a GLib.SpawnChildSetupFunc ` +
                'is unsafe and may dead-lock, thus it should never be used from JavaScript. ' +
                `Shell.${func} can be used to perform default actions or an ` +
                'async-signal-safe alternative should be used instead'));
            return originalFunc(...commonArgs, childSetup, ...args);
        }

        const retValue = Shell[`util_${func}`](...commonArgs, ...args);
        return [true, ...Array.isArray(retValue) ? retValue : [retValue]];
    };
}
GLib.spawn_async = wrapSpawnFunction('spawn_async');
GLib.spawn_async_with_pipes = wrapSpawnFunction('spawn_async_with_pipes');
GLib.spawn_async_with_fds = wrapSpawnFunction('spawn_async_with_fds');
GLib.spawn_async_with_pipes_and_fds = wrapSpawnFunction('spawn_async_with_pipes_and_fds');

// OK, now things are initialized enough that we can import shell JS
const Format = imports.format;

String.prototype.format = Format.format;

Math.clamp = function (x, lower, upper) {
    return Math.min(Math.max(x, lower), upper);
};

// Prevent extensions from opening a display connection to ourselves
Gdk.set_allowed_backends('');
