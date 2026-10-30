#!/usr/bin/env python3

import argparse
import sys
import os
import dbus
import pwd
import time
#import dbus.lowlevel
import dbusmock

from mutter_dbusrunner import MutterDBusRunner

class MockDisplayManager:
    BUS_NAME = 'org.gnome.DisplayManager'
    MAIN_OBJ = '/org/gnome/DisplayManager/Manager'
    MAIN_IFACE = 'org.gnome.DisplayManager.Manager'
    SYSTEM_BUS = True

    @staticmethod
    def load(mock, parameters={}):
        mock.AddMethods(MockDisplayManager.MAIN_IFACE,
                        [('RegisterSession', 'a{sv}', '', '')])

class MockPermissionStore:
    BUS_NAME = 'org.freedesktop.impl.portal.PermissionStore'
    MAIN_OBJ = '/org/freedesktop/impl/portal/PermissionStore'
    MAIN_IFACE = 'org.freedesktop.impl.portal.PermissionStore'
    SYSTEM_BUS = False

    @staticmethod
    def load(mock, parameters={}):
        mock.AddMethod(MockPermissionStore.MAIN_IFACE, 'Lookup', 'ss', 'a{sas}v',
            'ret = (dbus.Dictionary([], signature="sas"), dbus.String("NONE", variant_level=2))')

    @staticmethod
    def lookup_permission(self, table, id):
      return dbus.Struct([])

class MockSessionManager:
    BUS_NAME = 'org.gnome.SessionManager'
    MAIN_OBJ = '/org/gnome/SessionManager'
    MAIN_IFACE = 'org.gnome.SessionManager'
    SYSTEM_BUS = False

    @staticmethod
    def load(mock, parameters={}):
        mock.AddMethods(MockSessionManager.MAIN_IFACE,
                        [('Setenv', 'ss', '', '')])

class MockGeoClue:
    BUS_NAME = 'org.freedesktop.GeoClue2'
    MAIN_OBJ = '/org/freedesktop/GeoClue2/Manager'
    MAIN_IFACE = 'org.freedesktop.GeoClue2.Manager'
    SYSTEM_BUS = True

    @staticmethod
    def load(mock, parameters={}):
        mock.AddMethods(MockGeoClue.MAIN_IFACE,
                        [('AddAgent', 's', '', '')])

class MockGnomeShellCalendarServer:
    BUS_NAME = 'org.gnome.Shell.CalendarServer'
    MAIN_OBJ = '/org/gnome/Shell/CalendarServer'
    MAIN_IFACE = 'org.gnome.Shell.CalendarServer'
    SYSTEM_BUS = False

    @staticmethod
    def load(mock, parameters={}):
        mock.AddMethods(MockGnomeShellCalendarServer.MAIN_IFACE,
                        [('SetTimeRange', 'xxb', '', '')])

class MockParentalControls:
    BUS_NAME = 'com.endlessm.ParentalControls.AccountInfo'
    MAIN_OBJ = '/com/endlessm/ParentalControls/AccountInfo'
    MAIN_IFACE = 'com.endlessm.ParentalControls.AccountInfo'
    SYSTEM_BUS = False

    @staticmethod
    def load(mock, parameters={}):
        pass


class GnomeShellDBusRunner(MutterDBusRunner):
    @classmethod
    def setUpClass(klass, **kargs):
        MutterDBusRunner.setUpClass(**kargs)

        klass.add_template_dir(os.path.join(os.path.dirname(__file__),
                                            'dbusmock-templates'))

        systemd_system = klass.start_from_template('systemd', system_bus=True)
        systemd_user = klass.start_from_template('systemd', system_bus=False)
        klass.start_from_template('upower')
        klass.start_from_template('networkmanager')
        klass.start_from_template('polkitd')
        klass.start_from_template('power_profiles_daemon')

        if os.getenv('GNOME_SHELL_SESSION_MODE') == 'gdm':
            klass.start_greeter_mock()
        else:
            accounts_service = klass.start_from_local_template('accounts_service')
            empty_dict = dbus.Dictionary({}, signature='sv')
            accounts_service[1].AddUser(os.getuid(),
                                        pwd.getpwuid(os.getuid()).pw_name,
                                        '',
                                        empty_dict)
            klass.start_from_class(MockDisplayManager)

        klass.start_from_class(MockSessionManager)
        klass.start_from_class(MockGeoClue)
        klass.start_from_class(MockPermissionStore)
        klass.start_from_class(MockParentalControls)
        klass.start_from_class(MockGnomeShellCalendarServer)

        systemd_mock = dbus.Interface(systemd_user[1], dbusmock.MOCK_IFACE)
        systemd_mock.AddMockUnit('org.freedesktop.IBus.session.GNOME.service')
        systemd_mock.AddMockUnit('org.gnome.Shell.CalendarServer.service')

    @classmethod
    def start_greeter_mock(klass):
        # The login screen talks to GDM through the peer-to-peer channels it
        # opens, which dbusmock cannot serve: tests/greeter-mock.py stands in
        # for GDM and AccountsService, and logs what it is asked to
        # GREETER_MOCK_LOG, which the test reads (the test inherits the
        # environment of the runner)
        os.environ['GREETER_MOCK_LOG'] = os.path.join(os.environ['TMPDIR'],
                                                      'greeter-mock.log')
        klass.launch_service([sys.executable,
                              os.path.join(os.path.dirname(__file__),
                                           'greeter-mock.py')])

        names = ['org.gnome.DisplayManager', 'org.freedesktop.Accounts']
        deadline = time.monotonic() + 10
        while not all(klass.system_bus_con.name_has_owner(name) for name in names):
            if time.monotonic() > deadline:
                raise Exception('tests/greeter-mock.py did not start')
            time.sleep(0.1)
