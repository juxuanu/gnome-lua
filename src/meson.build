src_builddir = meson.current_build_dir()

service_data = configuration_data()
service_data.set('libexecdir', libexecdir)

introspection_common = {
  'install_gir': false,
  'install_typelib': true,
  'install_dir_typelib': pkglibdir,
  'fatal_warnings': get_option('werror'),
}

subdir('calendar-server')
subdir('hotplug-sniffer')
subdir('st')
if have_xwayland
  subdir('tray')
endif

script_data = configuration_data()
script_data.set('bindir', bindir)
script_data.set('datadir', datadir)
script_data.set('libdir', libdir)
script_data.set('libexecdir', libexecdir)
script_data.set('pkgdatadir', pkgdatadir)
script_data.set('pkglibdir', pkglibdir)
script_data.set('PYTHON', python.full_path())
script_data.set('VERSION', meson.project_version())

test_tool = configure_file(
  input: 'gnome-shell-test-tool.in',
  output: 'gnome-shell-test-tool',
  configuration: script_data,
  install_dir: bindir
)

if get_option('extensions_tool')
  configure_file(
    input: 'gnome-shell-extension-tool.in',
    output: 'gnome-shell-extension-tool',
    configuration: script_data,
    install_dir: bindir
  )
endif

typelib_cflags = [
  '-DMUTTER_TYPELIB_DIR="@0@"'.format(mutter_typelibdir),
  '-DSHELL_TYPELIB_DIR="@0@"'.format(pkglibdir),
]

gnome_shell_cflags = [
  '-DCLUTTER_ENABLE_EXPERIMENTAL_API',
  '-DCOGL_ENABLE_EXPERIMENTAL_API',
  '-DVERSION="@0@"'.format(meson.project_version()),
  '-DLOCALEDIR="@0@"'.format(localedir),
  '-DDATADIR="@0@"'.format(datadir),
  '-DGNOME_SHELL_LIBEXECDIR="@0@"'.format(libexecdir),
  '-DGNOME_SHELL_DATADIR="@0@"'.format(pkgdatadir),
  '-DGNOME_SHELL_PKGLIBDIR="@0@"'.format(pkglibdir)
]

install_rpath = ':'.join([mutter_typelibdir, pkglibdir])

gnome_shell_deps = [
  gio_unix_dep,
  libxml_dep,
  atk_bridge_dep,
  atspi_dep,
  gdk_pixbuf_dep,
  gjs_dep,
  mtk_dep,
  clutter_dep,
  gi_dep,
  polkit_dep,
  gcr_dep,
  libsystemd_dep,
  libpipewire_dep,
  pango_dep,
]

if have_xwayland
  gnome_shell_deps += x11_dep
endif

gnome_shell_deps += nm_deps

tools_cflags = '-DLOCALEDIR="@0@"'.format(localedir)
tools_deps = [gio_dep, gjs_dep]

libshell_menu_sources = [
  'gtkactionmuxer.h',
  'gtkactionmuxer.c',
  'gtkactionobservable.h',
  'gtkactionobservable.c',
  'gtkactionobserver.h',
  'gtkactionobserver.c'
]

libshell_menu = library('gnome-shell-menu',
  sources: libshell_menu_sources,
  dependencies: [gio_dep, clutter_dep, mtk_dep],
  include_directories: conf_inc,
  build_rpath: mutter_typelibdir,
  install_rpath: mutter_typelibdir,
  install_dir: pkglibdir,
  install: true
)

libshell_menu_dep = declare_dependency(link_with: libshell_menu)

libshell_public_headers = [
  'shell-app.h',
  'shell-app-system.h',
  'shell-app-usage.h',
  'shell-blur-effect.h',
  'ui/background.h',
  'ui/message-tray.h',
  'ui/notification-daemon.h',
  'ui/window-attention-handler.h',
  'ui/components/automount-manager.h',
  'ui/components/autorun-manager.h',
  'ui/components/keyring.h',
  'ui/components/polkit-agent.h',
  'ui/message-list.h',
  'misc/text-utils.h',
  'ui/bar-level.h',
  'ui/box-pointer.h',
  'ui/popup-menu.h',
  'ui/slider.h',
  'ui/ripples.h',
  'ui/check-box.h',
  'ui/dialog.h',
  'ui/lightbox.h',
  'ui/modal-dialog.h',
  'ui/locate-pointer.h',
  'ui/animation.h',
  'ui/pointer-a11y-timeout.h',
  'ui/focus-caret-tracker.h',
  'ui/magnifier.h',
  'ui/xdnd-handler.h',
  'ui/dnd.h',
  'ui/main.h',
  'ui/grab-helper.h',
  'ui/session-mode.h',
  'ui/osd-window.h',
  'ui/osd-monitor-labeler.h',
  'ui/page-indicators.h',
  'ui/workspace-switcher-popup.h',
  'ui/layout.h',
  'ui/swipe-tracker.h',
  'ui/keyboard.h',
  'ui/ibus-candidate-popup.h',
  'ui/kbd-a11y-dialog.h',
  'ui/background-menu.h',
  'ui/app-favorites.h',
  'ui/close-dialog.h',
  'ui/window-menu.h',
  'ui/panel-menu.h',
  'ui/panel.h',
  'ui/calendar.h',
  'ui/mpris.h',
  'ui/date-menu.h',
  'ui/quick-settings.h',
  'ui/switcher-popup.h',
  'ui/ctrl-alt-tab.h',
  'ui/switch-monitor.h',
  'ui/icon-grid.h',
  'ui/alt-tab.h',
  'ui/shell-entry.h',
  'ui/run-dialog.h',
  'ui/app-menu.h',
  'ui/search.h',
  'ui/remote-search.h',
  'ui/search-controller.h',
  'ui/pad-osd.h',
  'ui/inhibit-shortcuts-dialog.h',
  'ui/user-widget.h',
  'ui/qr-code.h',
  'ui/welcome-dialog.h',
  'ui/end-session-dialog.h',
  'ui/access-dialog.h',
  'ui/audio-device-selection.h',
  'ui/shell-mount-operation-ui.h',
  'ui/window-manager.h',
  'ui/workspace-animation.h',
  'ui/window-preview.h',
  'ui/workspace.h',
  'ui/workspace-thumbnail.h',
  'ui/app-display.h',
  'ui/dash.h',
  'ui/overview.h',
  'ui/overview-controls.h',
  'ui/workspaces-view.h',
  'gdm/batch.h',
  'gdm/credential-manager.h',
  'gdm/realmd.h',
  'gdm/settings.h',
  'gdm/smartcard-manager.h',
  'gdm/user-verifier.h',
  'gdm/util.h',
  'shell-camera-monitor.h',
  'shell-edge-drag-gesture.h',
  'shell-global.h',
  'shell-invert-lightness-effect.h',
  'shell-action-modes.h',
  'misc/gnome-session.h',
  'misc/ibus-manager.h',
  'misc/input-method.h',
  'misc/keyboard-manager.h',
  'misc/login-manager.h',
  'misc/history.h',
  'misc/permission-store.h',
  'misc/system-actions.h',
  'misc/dbus-object-manager.h',
  'misc/parental-controls-manager.h',
  'misc/weather.h',
  'misc/brightness-manager.h',
  'misc/break-manager.h',
  'misc/time-limits-manager.h',
  'misc/introspect.h',
  'misc/dbus-utils.h',
  'misc/date-utils.h',
  'ui/shell-dbus.h',
  'ui/screenshot-ui.h',
  'ui/status/accessibility.h',
  'ui/status/auto-rotate.h',
  'ui/status/background-apps.h',
  'ui/status/backlight.h',
  'ui/status/brightness.h',
  'ui/status/camera.h',
  'ui/status/dark-mode.h',
  'ui/status/do-not-disturb.h',
  'ui/status/dwell-click.h',
  'ui/status/keyboard.h',
  'ui/status/location.h',
  'ui/status/night-light.h',
  'ui/status/power-profiles.h',
  'ui/status/remote-access.h',
  'ui/status/rfkill.h',
  'ui/status/system.h',
  'ui/status/thunderbolt.h',
  'ui/status/volume.h',
  'shell-mount-operation.h',
  'shell-perf-log.h',
  'shell-screenshot.h',
  'shell-square-bin.h',
  'shell-stack.h',
  'shell-time-change-source.h',
  'shell-util.h',
  'shell-window-preview-layout.h',
  'shell-window-tracker.h',
  'shell-wm.h',
  'shell-workspace-background.h',
  'ui/environment.h'
]

if have_xwayland
  libshell_public_headers += [
    'shell-tray-icon.h',
    'shell-tray-manager.h',
  ]
endif

if have_networkmanager
  libshell_public_headers += [
    'shell-network-agent.h',
    'misc/modem-manager.h',
    'ui/components/network-agent.h',
    'ui/status/network.h',
  ]
endif

if have_bluetooth
  libshell_public_headers += ['ui/status/bluetooth.h']
endif

libshell_private_headers = [
  'shell-app-private.h',
  'shell-app-cache-private.h',
  'shell-app-system-private.h',
  'shell-global-private.h',
  'shell-wm-private.h',
  'gdm/auth-services.h',
  'gdm/auth-services-legacy.h',
  'gdm/auth-services-sssd-switchable.h',
  'gdm/fido2-token-manager.h',
  'gdm/fingerprint-manager.h',
  'ui/deferred-work.h'
]

if have_xwayland
  libshell_private_headers += [
    'shell-tray-icon-private.h',
  ]
endif

libshell_sources = [
  'gnome-shell-plugin.c',
  'shell-app.c',
  'shell-app-system.c',
  'shell-app-usage.c',
  'shell-blur-effect.c',
  'ui/background.c',
  'ui/message-tray.c',
  'ui/notification-daemon.c',
  'ui/window-attention-handler.c',
  'ui/components/automount-manager.c',
  'ui/components/autorun-manager.c',
  'ui/components/keyring.c',
  'ui/components/polkit-agent.c',
  'ui/message-list.c',
  'misc/text-utils.c',
  'ui/bar-level.c',
  'ui/box-pointer.c',
  'ui/popup-menu.c',
  'ui/slider.c',
  'ui/ripples.c',
  'ui/check-box.c',
  'ui/dialog.c',
  'ui/lightbox.c',
  'ui/modal-dialog.c',
  'ui/locate-pointer.c',
  'ui/animation.c',
  'ui/pointer-a11y-timeout.c',
  'ui/focus-caret-tracker.c',
  'ui/magnifier.c',
  'ui/xdnd-handler.c',
  'ui/dnd.c',
  'ui/main.c',
  'ui/grab-helper.c',
  'ui/session-mode.c',
  'ui/osd-window.c',
  'ui/osd-monitor-labeler.c',
  'ui/page-indicators.c',
  'ui/workspace-switcher-popup.c',
  'ui/layout.c',
  'ui/swipe-tracker.c',
  'ui/keyboard.c',
  'ui/ibus-candidate-popup.c',
  'ui/kbd-a11y-dialog.c',
  'ui/background-menu.c',
  'ui/app-favorites.c',
  'ui/close-dialog.c',
  'ui/window-menu.c',
  'ui/panel-menu.c',
  'ui/panel.c',
  'ui/calendar.c',
  'ui/mpris.c',
  'ui/date-menu.c',
  'ui/quick-settings.c',
  'ui/switcher-popup.c',
  'ui/ctrl-alt-tab.c',
  'ui/switch-monitor.c',
  'ui/icon-grid.c',
  'ui/alt-tab.c',
  'ui/shell-entry.c',
  'ui/run-dialog.c',
  'ui/app-menu.c',
  'ui/search.c',
  'ui/remote-search.c',
  'ui/search-controller.c',
  'ui/pad-osd.c',
  'ui/inhibit-shortcuts-dialog.c',
  'ui/user-widget.c',
  'ui/qr-code.c',
  'ui/welcome-dialog.c',
  'ui/end-session-dialog.c',
  'ui/access-dialog.c',
  'ui/audio-device-selection.c',
  'ui/shell-mount-operation-ui.c',
  'ui/window-manager.c',
  'ui/workspace-animation.c',
  'ui/window-preview.c',
  'ui/workspace.c',
  'ui/workspace-thumbnail.c',
  'ui/app-display.c',
  'ui/dash.c',
  'ui/overview.c',
  'ui/overview-controls.c',
  'ui/workspaces-view.c',
  'gdm/batch.c',
  'gdm/credential-manager.c',
  'gdm/realmd.c',
  'gdm/smartcard-manager.c',
  'gdm/user-verifier.c',
  'gdm/util.c',
  'shell-camera-monitor.c',
  'misc/gnome-session.c',
  'misc/ibus-manager.c',
  'misc/input-method.c',
  'misc/keyboard-manager.c',
  'misc/login-manager.c',
  'misc/history.c',
  'misc/permission-store.c',
  'misc/system-actions.c',
  'misc/dbus-object-manager.c',
  'misc/parental-controls-manager.c',
  'misc/weather.c',
  'misc/brightness-manager.c',
  'misc/break-manager.c',
  'misc/time-limits-manager.c',
  'misc/introspect.c',
  'misc/dbus-utils.c',
  'misc/date-utils.c',
  'ui/shell-dbus.c',
  'ui/screenshot-ui.c',
  'ui/status/accessibility.c',
  'ui/status/auto-rotate.c',
  'ui/status/background-apps.c',
  'ui/status/backlight.c',
  'ui/status/brightness.c',
  'ui/status/camera.c',
  'ui/status/dark-mode.c',
  'ui/status/do-not-disturb.c',
  'ui/status/dwell-click.c',
  'ui/status/keyboard.c',
  'ui/status/location.c',
  'ui/status/night-light.c',
  'ui/status/power-profiles.c',
  'ui/status/remote-access.c',
  'ui/status/rfkill.c',
  'ui/status/system.c',
  'ui/status/thunderbolt.c',
  'ui/status/volume.c',
  'shell-edge-drag-gesture.c',
  'shell-global.c',
  'shell-invert-lightness-effect.c',
  'shell-keyring-prompt.c',
  'shell-keyring-prompt.h',
  'shell-mount-operation.c',
  'shell-perf-log.c',
  'shell-polkit-authentication-agent.c',
  'shell-polkit-authentication-agent.h',
  'shell-screenshot.c',
  'shell-secure-text-buffer.c',
  'shell-secure-text-buffer.h',
  'shell-square-bin.c',
  'shell-stack.c',
  'shell-time-change-source.c',
  'shell-util.c',
  'shell-window-preview-layout.c',
  'shell-window-tracker.c',
  'shell-wm.c',
  'shell-workspace-background.c',
  'ui/environment.c'
]

if have_xwayland
  libshell_sources += [
    'shell-tray-icon.c',
    'shell-tray-manager.c',
  ]
endif

if have_networkmanager
  libshell_sources += [
    'shell-network-agent.c',
    'misc/modem-manager.c',
    'ui/components/network-agent.c',
    'ui/status/network.c',
  ]
endif

if have_bluetooth
  libshell_sources += ['ui/status/bluetooth.c']
endif

libshell_private_sources = [
  'shell-app-cache.c',
  'gdm/auth-services.c',
  'gdm/auth-services-legacy.c',
  'gdm/auth-services-sssd-switchable.c',
  'gdm/fido2-token-manager.c',
  'gdm/fingerprint-manager.c',
  'ui/deferred-work.c',
]

libshell_enums = gnome.mkenums_simple('shell-enum-types',
  sources: libshell_public_headers
)

# Proxies handed out by the ported misc modules to the remaining JS, so they
# are part of the Shell GIR (unlike dbus_generated below, which is C only).
libshell_login1_dbus = gnome.gdbus_codegen('login1-dbus',
  sources: [
    '../data/dbus-interfaces/org.freedesktop.login1.Manager.xml',
    '../data/dbus-interfaces/org.freedesktop.login1.Session.xml',
    '../data/dbus-interfaces/org.freedesktop.login1.User.xml',
  ],
  interface_prefix: 'org.freedesktop.',
  namespace: 'Shell'
)

libshell_gnome_session_dbus = gnome.gdbus_codegen('gnome-session-dbus',
  sources: [
    '../data/dbus-interfaces/org.gnome.SessionManager.xml',
    '../data/dbus-interfaces/org.gnome.SessionManager.Presence.xml',
    '../data/dbus-interfaces/org.gnome.SessionManager.Inhibitor.xml',
  ],
  interface_prefix: 'org.gnome.',
  namespace: 'Shell'
)

# The interface prefix is org.freedesktop.impl. (type ShellPortalPermissionStore)
# so that shell_permission_store_new () is a function of the namespace, not a
# constructor GIR would attach to the interface type.
libshell_permission_store_dbus = gnome.gdbus_codegen('permission-store-dbus',
  sources: '../data/dbus-interfaces/org.freedesktop.impl.portal.PermissionStore.xml',
  interface_prefix: 'org.freedesktop.impl.',
  namespace: 'Shell'
)

libshell_gir_sources = [
  libshell_enums,
  libshell_public_headers,
  libshell_login1_dbus,
  libshell_gnome_session_dbus,
  libshell_permission_store_dbus,
  libshell_sources
]

libshell_no_gir_sources = [
  js_resources,
  libshell_private_headers,
  libshell_private_sources
]

dbus_generated = gnome.gdbus_codegen('org-gtk-application',
  'org.gtk.Application.xml',
  namespace: 'Shell'
)

dbus_generated += gnome.gdbus_codegen('switcheroo-control',
  '../data/dbus-interfaces/net.hadess.SwitcherooControl.xml',
  namespace: 'Shell'
)

# Private proxies of misc/weather.c and misc/modem-manager.c, and the
# skeleton of misc/introspect.c
dbus_generated += gnome.gdbus_codegen('weather-integration-dbus',
  sources: '../data/dbus-interfaces/org.gnome.Shell.WeatherIntegration.xml',
  interface_prefix: 'org.gnome.Shell.',
  namespace: 'Shell'
)

# The proxies of ui/calendar.c (type name ShellCalendarServer) and of
# ui/date-menu.c (type name ShellClocksIntegration)
dbus_generated += gnome.gdbus_codegen('calendar-server-dbus',
  sources: '../data/dbus-interfaces/org.gnome.Shell.CalendarServer.xml',
  interface_prefix: 'org.gnome.Shell.',
  namespace: 'Shell'
)

dbus_generated += gnome.gdbus_codegen('clocks-integration-dbus',
  sources: '../data/dbus-interfaces/org.gnome.Shell.ClocksIntegration.xml',
  interface_prefix: 'org.gnome.Shell.',
  namespace: 'Shell'
)

# The proxies of ui/mpris.c (types ShellMediaPlayer2 and ShellMediaPlayer2Player)
dbus_generated += gnome.gdbus_codegen('mpris-dbus',
  sources: [
    '../data/dbus-interfaces/org.mpris.MediaPlayer2.xml',
    '../data/dbus-interfaces/org.mpris.MediaPlayer2.Player.xml',
  ],
  interface_prefix: 'org.mpris.',
  namespace: 'Shell'
)

dbus_generated += gnome.gdbus_codegen('introspect-dbus',
  sources: '../data/dbus-interfaces/org.gnome.Shell.Introspect.xml',
  interface_prefix: 'org.gnome.Shell.',
  namespace: 'Shell'
)

# The skeleton of ui/pad-osd.c (type name ShellWacomPadOsd)
dbus_generated += gnome.gdbus_codegen('pad-osd-dbus',
  sources: '../data/dbus-interfaces/org.gnome.Shell.PadOsd.xml',
  interface_prefix: 'org.gnome.Shell.',
  namespace: 'Shell'
)

# The proxies of ui/remote-search.c (type names ShellDBusSearchProvider and
# ShellDBusSearchProvider2)
dbus_generated += gnome.gdbus_codegen('search-provider-dbus',
  sources: [
    '../data/dbus-interfaces/org.gnome.ShellSearchProvider.xml',
    '../data/dbus-interfaces/org.gnome.ShellSearchProvider2.xml',
  ],
  interface_prefix: 'org.gnome.Shell.',
  namespace: 'ShellDBus'
)

# The skeletons of ui/shell-dbus.c (type names ShellDBusShell,
# ShellDBusShellExtensions, ShellDBusScreenSaver, ...)
dbus_generated += gnome.gdbus_codegen('shell-dbus-generated',
  sources: [
    '../data/dbus-interfaces/org.gnome.Shell.xml',
    '../data/dbus-interfaces/org.gnome.Shell.Extensions.xml',
    '../data/dbus-interfaces/org.gnome.ScreenSaver.xml',
    '../data/dbus-interfaces/org.gnome.Shell.ScreenTime.xml',
    '../data/dbus-interfaces/org.gnome.Shell.Brightness.xml',
  ],
  interface_prefix: 'org.gnome.',
  namespace: 'ShellDBus'
)

# The skeleton of the screenshot service and the proxy of the screencast
# service of ui/screenshot-ui.c (types ShellDBusScreenshot and
# ShellDBusScreencast; ShellScreenshot is the backend)
dbus_generated += gnome.gdbus_codegen('screenshot-dbus',
  sources: [
    '../data/dbus-interfaces/org.gnome.Shell.Screenshot.xml',
    '../data/dbus-interfaces/org.gnome.Shell.Screencast.xml',
  ],
  interface_prefix: 'org.gnome.Shell.',
  namespace: 'ShellDBus'
)

# The skeletons of ui/notification-daemon.c (types ShellFreedesktopNotifications
# and ShellGtkNotifications)
dbus_generated += gnome.gdbus_codegen('notifications-dbus',
  sources: [
    '../data/dbus-interfaces/org.freedesktop.Notifications.xml',
    '../data/dbus-interfaces/org.gtk.Notifications.xml',
  ],
  interface_prefix: 'org.',
  namespace: 'Shell'
)

# The proxy of ui/components/autorun-manager.c (type ShellHotplugSniffer)
dbus_generated += gnome.gdbus_codegen('hotplug-sniffer-dbus',
  sources: '../data/dbus-interfaces/org.gnome.Shell.HotplugSniffer.xml',
  interface_prefix: 'org.gnome.Shell.',
  namespace: 'Shell'
)

# The proxies of ui/status/night-light.c (type ShellSettingsDaemonColor),
# ui/status/rfkill.c (ShellSettingsDaemonRfkill), ui/status/backlight.c
# (ShellSettingsDaemonPowerKeyboard), ui/status/power-profiles.c
# (ShellPowerProfiles), ui/status/background-apps.c (ShellBackgroundMonitor)
# and ui/status/thunderbolt.c (ShellBolt1Manager and ShellBolt1Device)
dbus_generated += gnome.gdbus_codegen('night-light-dbus',
  sources: '../data/dbus-interfaces/org.gnome.SettingsDaemon.Color.xml',
  interface_prefix: 'org.gnome.',
  namespace: 'Shell'
)

dbus_generated += gnome.gdbus_codegen('rfkill-dbus',
  sources: '../data/dbus-interfaces/org.gnome.SettingsDaemon.Rfkill.xml',
  interface_prefix: 'org.gnome.',
  namespace: 'Shell'
)

dbus_generated += gnome.gdbus_codegen('backlight-dbus',
  sources: '../data/dbus-interfaces/org.gnome.SettingsDaemon.Power.Keyboard.xml',
  interface_prefix: 'org.gnome.',
  namespace: 'Shell'
)

dbus_generated += gnome.gdbus_codegen('power-profiles-dbus',
  sources: '../data/dbus-interfaces/org.freedesktop.UPower.PowerProfiles.xml',
  interface_prefix: 'org.freedesktop.UPower.',
  namespace: 'Shell'
)

dbus_generated += gnome.gdbus_codegen('background-monitor-dbus',
  sources: '../data/dbus-interfaces/org.freedesktop.background.Monitor.xml',
  interface_prefix: 'org.freedesktop.',
  namespace: 'Shell'
)

# The proxies of gdm/credential-manager.c (types ShellOvirtCredentials and
# ShellVmwareCredentials), gdm/realmd.c (ShellRealmdProvider, ShellRealmdService
# and ShellRealmdRealm) and gdm/fingerprint-manager.c (ShellFprintManager and
# ShellFprintDevice)
dbus_generated += gnome.gdbus_codegen('ovirt-dbus',
  sources: '../data/dbus-interfaces/org.ovirt.vdsm.Credentials.xml',
  interface_prefix: 'org.ovirt.vdsm.',
  namespace: 'ShellOvirt'
)

dbus_generated += gnome.gdbus_codegen('vmware-dbus',
  sources: '../data/dbus-interfaces/org.vmware.viewagent.Credentials.xml',
  interface_prefix: 'org.vmware.viewagent.',
  namespace: 'ShellVmware'
)

dbus_generated += gnome.gdbus_codegen('realmd-dbus',
  sources: [
    '../data/dbus-interfaces/org.freedesktop.realmd.Provider.xml',
    '../data/dbus-interfaces/org.freedesktop.realmd.Service.xml',
    '../data/dbus-interfaces/org.freedesktop.realmd.Realm.xml',
  ],
  interface_prefix: 'org.freedesktop.',
  namespace: 'Shell'
)

dbus_generated += gnome.gdbus_codegen('fprint-dbus',
  sources: [
    '../data/dbus-interfaces/net.reactivated.Fprint.Manager.xml',
    '../data/dbus-interfaces/net.reactivated.Fprint.Device.xml',
  ],
  interface_prefix: 'net.reactivated.',
  namespace: 'Shell'
)

dbus_generated += gnome.gdbus_codegen('bolt-dbus',
  sources: [
    '../data/dbus-interfaces/org.freedesktop.bolt1.Manager.xml',
    '../data/dbus-interfaces/org.freedesktop.bolt1.Device.xml',
  ],
  interface_prefix: 'org.freedesktop.',
  namespace: 'Shell'
)

# The skeletons and proxies of ui/access-dialog.c (types ShellPortalAccess and
# ShellPortalRequest), ui/end-session-dialog.c
# (ShellSessionManagerEndSessionDialog and ShellSoftwareOfflineUpdates),
# ui/audio-device-selection.c (ShellAudioDeviceSelection) and
# ui/shell-mount-operation-ui.c (ShellMountOperationHandler)
dbus_generated += gnome.gdbus_codegen('portal-access-dbus',
  sources: [
    '../data/dbus-interfaces/org.freedesktop.impl.portal.Access.xml',
    '../data/dbus-interfaces/org.freedesktop.impl.portal.Request.xml',
  ],
  interface_prefix: 'org.freedesktop.impl.',
  namespace: 'Shell'
)

dbus_generated += gnome.gdbus_codegen('end-session-dialog-dbus',
  sources: '../data/dbus-interfaces/org.gnome.SessionManager.EndSessionDialog.xml',
  interface_prefix: 'org.gnome.',
  namespace: 'Shell'
)

dbus_generated += gnome.gdbus_codegen('software-offline-updates-dbus',
  sources: '../data/dbus-interfaces/org.gnome.Software.OfflineUpdates.xml',
  interface_prefix: 'org.gnome.',
  namespace: 'Shell'
)

dbus_generated += gnome.gdbus_codegen('audio-device-selection-dbus',
  sources: '../data/dbus-interfaces/org.gnome.Shell.AudioDeviceSelection.xml',
  interface_prefix: 'org.gnome.Shell.',
  namespace: 'Shell'
)

dbus_generated += gnome.gdbus_codegen('mount-operation-handler-dbus',
  sources: '../data/dbus-interfaces/org.Gtk.MountOperationHandler.xml',
  interface_prefix: 'org.Gtk.',
  namespace: 'Shell'
)

# The skeleton and the proxy of ui/status/location.c (types ShellGeoClue2Agent
# and ShellGeoClue2Manager) and the proxy of ui/status/system.c
# (ShellUPowerDevice)
dbus_generated += gnome.gdbus_codegen('geoclue-dbus',
  sources: [
    '../data/dbus-interfaces/org.freedesktop.GeoClue2.Agent.xml',
    '../data/dbus-interfaces/org.freedesktop.GeoClue2.Manager.xml',
  ],
  interface_prefix: 'org.freedesktop.',
  namespace: 'Shell'
)

dbus_generated += gnome.gdbus_codegen('upower-device-dbus',
  sources: '../data/dbus-interfaces/org.freedesktop.UPower.Device.xml',
  interface_prefix: 'org.freedesktop.',
  namespace: 'Shell'
)

# The proxy of misc/time-limits-manager.c (type ShellMalcontentTimer1Child)
dbus_generated += gnome.gdbus_codegen('malcontent-timer-dbus',
  sources: '../data/dbus-interfaces/org.freedesktop.MalcontentTimer1.Child.xml',
  interface_prefix: 'org.freedesktop.',
  namespace: 'Shell'
)

# The proxy of ui/status/bluetooth.c (type ShellBluetoothRfkill). It has a
# namespace of its own: ui/status/rfkill.c has the same interface.
if have_bluetooth
  dbus_generated += gnome.gdbus_codegen('bluetooth-rfkill-dbus',
    sources: '../data/dbus-interfaces/org.gnome.SettingsDaemon.Rfkill.xml',
    interface_prefix: 'org.gnome.SettingsDaemon.',
    namespace: 'ShellBluetooth'
  )
endif

if have_networkmanager
  dbus_generated += gnome.gdbus_codegen('modem-manager-dbus',
    sources: [
      '../data/dbus-interfaces/org.freedesktop.ModemManager.Modem.Gsm.Network.xml',
      '../data/dbus-interfaces/org.freedesktop.ModemManager.Modem.Cdma.xml',
      '../data/dbus-interfaces/org.freedesktop.ModemManager1.Modem.xml',
      '../data/dbus-interfaces/org.freedesktop.ModemManager1.Modem.Modem3gpp.xml',
      '../data/dbus-interfaces/org.freedesktop.ModemManager1.Modem.ModemCdma.xml',
    ],
    interface_prefix: 'org.freedesktop.',
    namespace: 'Shell'
  )
endif

# The proxy of ui/status/network.c (type ShellPortalHelper); the captive
# portal login goes through the portal helper only if it is built
if have_portal_helper
  portal_helper_dbus = gnome.gdbus_codegen('portal-helper-dbus',
    sources: '../data/dbus-interfaces/org.gnome.Shell.PortalHelper.xml',
    interface_prefix: 'org.gnome.Shell.',
    namespace: 'Shell'
  )
  dbus_generated += portal_helper_dbus
endif

libshell_no_gir_sources += dbus_generated

libshell_link_with = []
if have_xwayland
  libshell_link_with += [libtray]
endif

libshell = library('shell-' + mutter_api_version,
  sources: libshell_gir_sources + libshell_no_gir_sources,
  dependencies: gnome_shell_deps + [libshell_menu_dep, libshell_lua_dep, libst_dep, mutter_dep, gnome_desktop_dep, gnome_bg_dep, gnome_qr_dep, glycin_dep, ibus_dep, json_glib_dep, m_dep, cairo_dep, accountsservice_dep, gdm_dep, gweather_dep, geoclue_dep, malcontent_dep, rsvg_dep, libgvc_dep, upower_dep, gudev_dep] + (have_bluetooth ? [bluetooth_dep] : []),
  include_directories: [conf_inc, include_directories('tray')],
  c_args: gnome_shell_cflags,
  link_with: libshell_link_with,
  build_rpath: mutter_typelibdir,
  install_rpath: install_rpath,
  install_dir: pkglibdir,
  install: true
)

libshell_dep = declare_dependency(
  link_with: libshell,
  dependencies: [libshell_lua_dep, libst_dep, mutter_dep],
  include_directories: [include_directories('tray')],
)

libshell_gir_includes = [
  'AccountsService-1.0',
  'Atspi-2.0',
  'Clutter-@0@'.format(mutter_api_version),
  'Meta-@0@'.format(mutter_api_version),
  'Gcr-4',
  'Gdm-1.0',
  'GioUnix-2.0',
  'GnomeDesktop-4.0',
  'IBus-1.0',
  'GWeather-4.0',
  'PolkitAgent-1.0',
  'GdkPixbuf-2.0'
]

if have_networkmanager
  libshell_gir_includes += ['NM-1.0']
endif

libshell_gir_includes += [
  libgvc_gir[0],
  libshell_lua_gir[0],
  libst_gir[0]
]

libshell_gir = gnome.generate_gir(libshell,
  sources: libshell_gir_sources,
  nsversion: mutter_api_version,
  namespace: 'Shell',
  includes: libshell_gir_includes,
  dependencies: [libgvc_dep],
  extra_args: ['--quiet'],
  kwargs: introspection_common,
)

gnome_shell_binary = executable('gnome-shell', 'main.c',
  c_args: gnome_shell_cflags + typelib_cflags,
  dependencies: gnome_shell_deps + [libshell_dep, libst_dep, mutter_dep],
  include_directories: [conf_inc],
  build_rpath: mutter_typelibdir,
  install_rpath: install_rpath,
  install: true
)

if have_portal_helper
  # The captive portal login window: a plain GTK application (the skeleton
  # of the D-Bus interface is compiled into it as well as into libshell)
  executable('gnome-shell-portal-helper',
    'portal-helper/main.c', portal_helper_dbus,
    c_args: tools_cflags,
    dependencies: [gio_dep, gtk_dep, webkitgtk_dep, libadwaita_dep],
    include_directories: [conf_inc],
    install_dir: libexecdir,
    install: true
  )
endif

gnome_shell_perf_helper = executable('gnome-shell-perf-helper', 'shell-perf-helper.c', js_resources,
  dependencies: [gtk_dep, gio_dep, m_dep],
  include_directories: [conf_inc],
  install_dir: libexecdir,
  install: true
)

run_test = executable('run-js-test', 'run-js-test.c',
  dependencies: [gio_dep, gi_dep, gjs_dep, libshell_dep],
  c_args: typelib_cflags,
  include_directories: [conf_inc],
  build_rpath: mutter_typelibdir,
)
