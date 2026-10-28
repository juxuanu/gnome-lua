import Gio from 'gi://Gio';
import Shell from 'gi://Shell';
import * as Signals from '../misc/signals.js';

const SmartcardTokenIface = `
<node>
<interface name="org.gnome.SettingsDaemon.Smartcard.Token">
  <property name="Name" type="s" access="read"/>
  <property name="Driver" type="o" access="read"/>
  <property name="IsInserted" type="b" access="read"/>
  <property name="UsedToLogin" type="b" access="read"/>
</interface>
</node>`;

const SMARTCARD_TOKEN_PROPERTIES = ['Name', 'Driver', 'IsInserted', 'UsedToLogin'];

/**
 * The proxies of the object manager are plain Gio.DBusProxy objects created
 * in C, so GJS did not add the accessors of the interface properties to them.
 *
 * @param {Gio.DBusProxy} token - a token proxy
 */
function _addTokenProperties(token) {
    for (const name of SMARTCARD_TOKEN_PROPERTIES) {
        Object.defineProperty(token, name, {
            configurable: true,
            get: () => token.get_cached_property(name)?.deepUnpack() ?? null,
        });
    }
}

let _smartcardManager = null;

/**
 * @returns {SmartcardManager}
 */
export function getSmartcardManager() {
    if (_smartcardManager == null)
        _smartcardManager = new SmartcardManager();

    return _smartcardManager;
}

class SmartcardManager extends Signals.EventEmitter {
    constructor() {
        super();

        this._objectManager = new Shell.DBusObjectManager({
            connection: Gio.DBus.session,
            name: 'org.gnome.SettingsDaemon.Smartcard',
            object_path: '/org/gnome/SettingsDaemon/Smartcard',
            known_interfaces: [SmartcardTokenIface],
        });
        this._objectManager.connect('loaded', this._onLoaded.bind(this));
        this._insertedTokens = {};
        this._loginToken = null;
    }

    _onLoaded() {
        const tokens = this._objectManager.get_proxies_for_interface('org.gnome.SettingsDaemon.Smartcard.Token');

        for (let i = 0; i < tokens.length; i++)
            this._addToken(tokens[i]);

        this._objectManager.connect('interface-added', (objectManager, interfaceName, proxy) => {
            if (interfaceName === 'org.gnome.SettingsDaemon.Smartcard.Token')
                this._addToken(proxy);
        });

        this._objectManager.connect('interface-removed', (objectManager, interfaceName, proxy) => {
            if (interfaceName === 'org.gnome.SettingsDaemon.Smartcard.Token')
                this._removeToken(proxy);
        });
    }

    _updateToken(token) {
        const objectPath = token.get_object_path();

        delete this._insertedTokens[objectPath];

        if (token.IsInserted)
            this._insertedTokens[objectPath] = token;

        if (token.UsedToLogin)
            this._loginToken = token;
    }

    _addToken(token) {
        // This is an alias, mirroring the real token.
        // Skip it to avoid duplicate events
        const objectPath = token.get_object_path();
        if (objectPath.endsWith('/login_token'))
            return;

        _addTokenProperties(token);
        this._updateToken(token);

        token.connectObject('g-properties-changed', (proxy, properties) => {
            const isInsertedChanged = !!properties.lookup_value('IsInserted', null);
            if (isInsertedChanged) {
                this._updateToken(token);

                if (token.IsInserted)
                    this.emit('smartcard-inserted', token);
                else
                    this.emit('smartcard-removed', token);
            }
        }, this);

        // Emit a smartcard-inserted at startup if it's already plugged in
        if (token.IsInserted)
            this.emit('smartcard-inserted', token);
    }

    _removeToken(token) {
        const objectPath = token.get_object_path();

        if (this._insertedTokens[objectPath] === token) {
            delete this._insertedTokens[objectPath];
            this.emit('smartcard-removed', token);
        }

        if (this._loginToken === token)
            this._loginToken = null;

        token.disconnectObject(this);
    }

    hasInsertedTokens() {
        return Object.keys(this._insertedTokens).length > 0;
    }

    hasInsertedLoginToken() {
        if (!this._loginToken)
            return false;

        if (!this._loginToken.IsInserted)
            return false;

        return true;
    }
}
