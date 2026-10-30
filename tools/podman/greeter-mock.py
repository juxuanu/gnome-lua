#!/usr/bin/python3
'''Stand-ins for the system services a GDM greeter talks to, for
tools/podman/run-nested.sh --mode=gdm.

On the system bus: GDM's manager, whose peer-to-peer channels lead to the
greeter's GDM session (its user verifier and greeter interfaces), and
AccountsService with two users. Verification takes a username when none was
chosen, then a password: "gnome" for every user, anything else fails. A
verified user's session is not started; the greeter is only told it opened.
No user has a session to reauthenticate, as at a real login screen.
'''

import os
import pwd
import sys

from gi.repository import Gio, GLib

PASSWORD = 'gnome'
GDM_VERSION = '51.1.1'
SESSION_PATH = '/org/gnome/DisplayManager/Session'
ACCOUNTS_PATH = '/org/freedesktop/Accounts'
PROPERTIES = 'org.freedesktop.DBus.Properties'

MANAGER_XML = '''
<node>
  <interface name="org.gnome.DisplayManager.Manager">
    <method name="RegisterDisplay"/>
    <method name="RegisterSession"/>
    <method name="OpenSession">
      <arg name="address" direction="out" type="s"/>
    </method>
    <method name="OpenReauthenticationChannel">
      <arg name="username" direction="in" type="s"/>
      <arg name="address" direction="out" type="s"/>
    </method>
    <property name="Version" type="s" access="read"/>
  </interface>
</node>'''

# What malcontent reads of a user: no restriction
PARENTAL_CONTROLS_XML = '''
<node>
  <interface name="com.endlessm.ParentalControls.AppFilter">
    <property name="AppFilter" type="(bas)" access="read"/>
    <property name="OarsFilter" type="(sa{ss})" access="read"/>
    <property name="AllowUserInstallation" type="b" access="read"/>
    <property name="AllowSystemInstallation" type="b" access="read"/>
  </interface>
  <interface name="com.endlessm.ParentalControls.SessionLimits">
    <property name="LimitType" type="u" access="read"/>
    <property name="DailySchedule" type="(uu)" access="read"/>
  </interface>
</node>'''
PARENTAL_CONTROLS = {
    'com.endlessm.ParentalControls.AppFilter': {
        'AppFilter': GLib.Variant('(bas)', (False, [])),
        'OarsFilter': GLib.Variant('(sa{ss})', ('oars-1.1', {})),
        'AllowUserInstallation': GLib.Variant('b', True),
        'AllowSystemInstallation': GLib.Variant('b', False),
    },
    'com.endlessm.ParentalControls.SessionLimits': {
        'LimitType': GLib.Variant('u', 0),
        'DailySchedule': GLib.Variant('(uu)', (0, 24 * 60 * 60)),
    },
}

# The interfaces of GDM's daemon/gdm-session.xml the greeter uses
SESSION_XML = '''
<node>
  <interface name="org.gnome.DisplayManager.UserVerifier">
    <method name="EnableExtensions"><arg name="extensions" direction="in" type="as"/></method>
    <method name="BeginVerification"><arg name="service_name" direction="in" type="s"/></method>
    <method name="BeginVerificationForUser">
      <arg name="service_name" direction="in" type="s"/>
      <arg name="username" direction="in" type="s"/>
    </method>
    <method name="AnswerQuery">
      <arg name="service_name" direction="in" type="s"/>
      <arg name="answer" direction="in" type="s"/>
    </method>
    <method name="Cancel"/>
    <signal name="ConversationStarted"><arg name="service_name" type="s"/></signal>
    <signal name="ConversationStopped"><arg name="service_name" type="s"/></signal>
    <signal name="ReauthenticationStarted"><arg name="pid_of_caller" type="i"/></signal>
    <signal name="Info"><arg name="service_name" type="s"/><arg name="info" type="s"/></signal>
    <signal name="Problem"><arg name="service_name" type="s"/><arg name="problem" type="s"/></signal>
    <signal name="InfoQuery"><arg name="service_name" type="s"/><arg name="query" type="s"/></signal>
    <signal name="SecretInfoQuery"><arg name="service_name" type="s"/><arg name="query" type="s"/></signal>
    <signal name="Reset"/>
    <signal name="ServiceUnavailable"><arg name="service_name" type="s"/><arg name="message" type="s"/></signal>
    <signal name="VerificationFailed"><arg name="service_name" type="s"/></signal>
    <signal name="VerificationComplete"><arg name="service_name" type="s"/></signal>
  </interface>
  <interface name="org.gnome.DisplayManager.UserVerifier.ChoiceList">
    <method name="SelectChoice">
      <arg name="service_name" direction="in" type="s"/>
      <arg name="choice" direction="in" type="s"/>
    </method>
    <signal name="ChoiceQuery">
      <arg name="service_name" type="s"/>
      <arg name="prompt_message" type="s"/>
      <arg name="list" type="a{ss}"/>
    </signal>
  </interface>
  <interface name="org.gnome.DisplayManager.Greeter">
    <method name="SelectSession"><arg name="session" direction="in" type="s"/></method>
    <method name="SelectUser"><arg name="username" direction="in" type="s"/></method>
    <method name="BeginAutoLogin"><arg name="username" direction="in" type="s"/></method>
    <method name="GetTimedLoginDetails">
      <arg name="enabled" direction="out" type="b"/>
      <arg name="username" direction="out" type="s"/>
      <arg name="delay" direction="out" type="i"/>
    </method>
    <method name="StartSessionWhenReady">
      <arg name="service_name" direction="in" type="s"/>
      <arg name="should_start_session" direction="in" type="b"/>
    </method>
    <method name="StopConflictingSession"/>
    <signal name="SelectedUserChanged"><arg name="username" type="s"/></signal>
    <signal name="DefaultLanguageNameChanged"><arg name="language_name" type="s"/></signal>
    <signal name="DefaultSessionNameChanged"><arg name="session_name" type="s"/></signal>
    <signal name="TimedLoginRequested"><arg name="username" type="s"/><arg name="delay" type="i"/></signal>
    <signal name="SessionOpened"><arg name="service_name" type="s"/><arg name="session_id" type="s"/></signal>
    <signal name="Reauthenticated"><arg name="service_name" type="s"/></signal>
  </interface>
</node>'''


def log(message):
    print(f'greeter-mock: {message}', file=sys.stderr, flush=True)


def default_value(signature):
    '''An empty value of a D-Bus type'''
    if signature.startswith('a'):
        return GLib.Variant(signature, [])
    if signature in ('s', 'o', 'g'):
        return GLib.Variant(signature, '/' if signature == 'o' else '')
    if signature == 'b':
        return GLib.Variant('b', False)
    return GLib.Variant(signature, 0)


def handle_properties(invocation, method_name, parameters, interfaces):
    '''Answers org.freedesktop.DBus.Properties from @interfaces
    ({interface: {name: GVariant}})'''
    values = interfaces.get(parameters.unpack()[0], {})
    if method_name == 'Get':
        _, name = parameters.unpack()
        if name in values:
            invocation.return_value(GLib.Variant('(v)', (values[name],)))
        else:
            invocation.return_dbus_error('org.freedesktop.DBus.Error.UnknownProperty', name)
    elif method_name == 'GetAll':
        invocation.return_value(GLib.Variant('(a{sv})', (values,)))
    else:
        invocation.return_dbus_error('org.freedesktop.DBus.Error.PropertyReadOnly', 'Read-only')


class Accounts:
    '''AccountsService with a fixed set of users'''

    def __init__(self, connection, users):
        accounts_info = self._interface('org.freedesktop.Accounts')
        user_info = self._interface('org.freedesktop.Accounts.User')
        self.users = {}

        parental_controls = Gio.DBusNodeInfo.new_for_xml(PARENTAL_CONTROLS_XML).interfaces

        for uid, (name, real_name) in enumerate(users, start=os.getuid()):
            path = f'{ACCOUNTS_PATH}/User{uid}'
            values = {p.name: default_value(p.signature) for p in user_info.properties}
            values.update({
                'Uid': GLib.Variant('t', uid),
                'UserName': GLib.Variant('s', name),
                'RealName': GLib.Variant('s', real_name),
                'AccountType': GLib.Variant('i', 1 if uid == os.getuid() else 0),
                'HomeDirectory': GLib.Variant('s', f'/home/{name}'),
                'Shell': GLib.Variant('s', '/bin/bash'),
                'LocalAccount': GLib.Variant('b', True),
                'LoginFrequency': GLib.Variant('t', 1),
                'Language': GLib.Variant('s', 'en_US.UTF-8'),
                'Session': GLib.Variant('s', 'gnome'),
                'SessionType': GLib.Variant('s', 'wayland'),
            })
            self.users[path] = {user_info.name: values, **PARENTAL_CONTROLS}
            for info in [user_info, *parental_controls]:
                connection.register_object(path, info, self._on_user_call, None, None)

        values = {p.name: default_value(p.signature) for p in accounts_info.properties}
        values.update({
            'DaemonVersion': GLib.Variant('s', '23.13'),
            'HasNoUsers': GLib.Variant('b', False),
            'HasMultipleUsers': GLib.Variant('b', len(self.users) > 1),
        })
        self.interfaces = {accounts_info.name: values}
        connection.register_object(ACCOUNTS_PATH, accounts_info, self._on_call, None, None)

    @staticmethod
    def _interface(name):
        path = f'/usr/share/dbus-1/interfaces/{name}.xml'
        with open(path, encoding='utf-8') as file:
            return Gio.DBusNodeInfo.new_for_xml(file.read()).lookup_interface(name)

    def _find(self, key, value):
        for path, interfaces in self.users.items():
            if interfaces['org.freedesktop.Accounts.User'][key].unpack() == value:
                return path
        return None

    def _on_call(self, connection, sender, path, interface, method, parameters, invocation):
        if interface == PROPERTIES:
            handle_properties(invocation, method, parameters, self.interfaces)
        elif method == 'ListCachedUsers':
            invocation.return_value(GLib.Variant('(ao)', (list(self.users),)))
        elif method in ('FindUserById', 'FindUserByName', 'CacheUser'):
            (value,) = parameters.unpack()
            found = self._find('Uid' if method == 'FindUserById' else 'UserName', value)
            if found:
                invocation.return_value(GLib.Variant('(o)', (found,)))
            else:
                invocation.return_dbus_error('org.freedesktop.Accounts.Error.Failed',
                                             f'No such user: {value}')
        else:
            invocation.return_dbus_error('org.freedesktop.Accounts.Error.NotSupported', method)

    def _on_user_call(self, connection, sender, path, interface, method, parameters, invocation):
        if interface == PROPERTIES:
            handle_properties(invocation, method, parameters, self.users[path])
        else:
            invocation.return_dbus_error('org.freedesktop.Accounts.Error.NotSupported', method)


class Session:
    '''The GDM session of the greeter: like GDM's, it is exported on every
    channel the greeter opens, and signals go to all of them'''

    def __init__(self, usernames):
        self.usernames = usernames
        self.connections = []
        self.conversations = {}  # service -> username, or None while asking it
        self.interfaces = Gio.DBusNodeInfo.new_for_xml(SESSION_XML).interfaces

    def add_connection(self, connection):
        for info in self.interfaces:
            connection.register_object(SESSION_PATH, info, self._on_call, None, None)
        self.connections.append(connection)
        connection.connect('closed', lambda c, *_: self.connections.remove(c))
        log(f'channel {len(self.connections)} opened')

    def _emit(self, interface, name, signature='', *args):
        for connection in self.connections:
            connection.emit_signal(None, SESSION_PATH, f'org.gnome.DisplayManager.{interface}', name,
                                   GLib.Variant(f'({signature})', args) if signature else None)

    def _begin(self, service, username):
        if service != 'gdm-password':
            self._emit('UserVerifier', 'ServiceUnavailable', 'ss', service,
                       f'The mock only does gdm-password, not {service}')
            return
        log(f'{service}: verifying {username or "a user to be named"}')
        self.conversations[service] = username
        self._emit('UserVerifier', 'ConversationStarted', 's', service)
        if username:
            self._emit('UserVerifier', 'SecretInfoQuery', 'ss', service, 'Password:')
        else:
            self._emit('UserVerifier', 'InfoQuery', 'ss', service, 'Username:')

    def _answer(self, service, answer):
        if service not in self.conversations:
            return
        username = self.conversations[service]
        if username is None:
            self.conversations[service] = answer
            self._emit('UserVerifier', 'SecretInfoQuery', 'ss', service, 'Password:')
        elif answer == PASSWORD and username in self.usernames:
            log(f'{service}: {username} verified')
            del self.conversations[service]
            self._emit('UserVerifier', 'VerificationComplete', 's', service)
            self._emit('Greeter', 'SessionOpened', 'ss', service, 'mock-session')
        else:
            log(f'{service}: {username} refused')
            del self.conversations[service]
            self._emit('UserVerifier', 'Problem', 'ss', service,
                       'Sorry, that didn’t work. Please try again.')
            self._emit('UserVerifier', 'VerificationFailed', 's', service)
            self._emit('UserVerifier', 'ConversationStopped', 's', service)

    def _on_call(self, connection, sender, path, interface, method, parameters, invocation):
        args = parameters.unpack() if parameters else ()
        if method == 'BeginVerification':
            self._begin(args[0], None)
        elif method == 'BeginVerificationForUser':
            self._begin(*args)
        elif method == 'AnswerQuery':
            self._answer(*args)
        elif method == 'Cancel':
            self.conversations.clear()
        elif method == 'GetTimedLoginDetails':
            invocation.return_value(GLib.Variant('(bsi)', (False, '', 0)))
            return
        elif method == 'StartSessionWhenReady':
            log(f'{args[0]}: the session would start now' if args[1] else
                f'{args[0]}: the session would not start')
        invocation.return_value(None)


class Manager:
    '''GDM's manager: hands out peer-to-peer channels to the session'''

    def __init__(self, connection, usernames):
        self.session = Session(usernames)
        self.server = Gio.DBusServer.new_sync('unix:tmpdir=/tmp', Gio.DBusServerFlags.NONE,
                                              Gio.dbus_generate_guid(), None, None)
        self.server.connect('new-connection', self._on_new_connection)
        self.server.start()
        info = Gio.DBusNodeInfo.new_for_xml(MANAGER_XML).interfaces[0]
        connection.register_object('/org/gnome/DisplayManager/Manager', info,
                                   self._on_call, None, None)

    def _on_new_connection(self, server, connection):
        self.session.add_connection(connection)
        return True

    def _on_call(self, connection, sender, path, interface, method, parameters, invocation):
        if interface == PROPERTIES:
            handle_properties(invocation, method, parameters,
                              {'org.gnome.DisplayManager.Manager':
                               {'Version': GLib.Variant('s', GDM_VERSION)}})
        elif method == 'OpenSession':
            invocation.return_value(GLib.Variant('(s)', (self.server.get_client_address(),)))
        elif method == 'OpenReauthenticationChannel':
            # What GDM answers for a user without a session, as the users of
            # the login screen are: the greeter then verifies them in its own
            # session
            invocation.return_dbus_error('org.freedesktop.DBus.Error.AccessDenied',
                                         'No session available')
        else:
            log(f'{method} called')
            invocation.return_value(None)


def main():
    me = pwd.getpwuid(os.getuid())
    users = [(me.pw_name, me.pw_gecos.split(',')[0] or me.pw_name.capitalize()),
             ('guest', 'Guest')]
    connection = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    usernames = [name for name, _ in users]
    keep = [Accounts(connection, users), Manager(connection, usernames)]

    for name in ('org.freedesktop.Accounts', 'org.gnome.DisplayManager'):
        Gio.bus_own_name_on_connection(connection, name, Gio.BusNameOwnerFlags.NONE, None, None)

    log(f'users {", ".join(usernames)}, password "{PASSWORD}"')
    GLib.MainLoop().run()
    del keep


if __name__ == '__main__':
    main()
