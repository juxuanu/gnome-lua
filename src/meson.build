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
  'ui/background-menu.h',
  'ui/panel-menu.h',
  'ui/panel.h',
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
  'misc/introspect.h',
  'shell-mount-operation.h',
  'shell-perf-log.h',
  'shell-screenshot.h',
  'shell-square-bin.h',
  'shell-stack.h',
  'shell-time-change-source.h',
  'shell-util.h',
  'shell-window-preview.h',
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
  libshell_public_headers += ['shell-network-agent.h', 'misc/modem-manager.h']
endif

libshell_private_headers = [
  'shell-app-private.h',
  'shell-app-cache-private.h',
  'shell-app-system-private.h',
  'shell-global-private.h',
  'shell-wm-private.h'
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
  'ui/background-menu.c',
  'ui/panel-menu.c',
  'ui/panel.c',
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
  'misc/introspect.c',
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
  'shell-window-preview.c',
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
  libshell_sources += ['shell-network-agent.c', 'misc/modem-manager.c']
endif

libshell_private_sources = [
  'shell-app-cache.c',
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

dbus_generated += gnome.gdbus_codegen('introspect-dbus',
  sources: '../data/dbus-interfaces/org.gnome.Shell.Introspect.xml',
  interface_prefix: 'org.gnome.Shell.',
  namespace: 'Shell'
)

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

libshell_no_gir_sources += dbus_generated

libshell_link_with = []
if have_xwayland
  libshell_link_with += [libtray]
endif

libshell = library('shell-' + mutter_api_version,
  sources: libshell_gir_sources + libshell_no_gir_sources,
  dependencies: gnome_shell_deps + [libshell_menu_dep, libshell_lua_dep, libst_dep, mutter_dep, gnome_desktop_dep, gnome_bg_dep, glycin_dep, ibus_dep, m_dep, cairo_dep, accountsservice_dep, gdm_dep, gweather_dep, geoclue_dep, malcontent_dep],
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
  'Clutter-@0@'.format(mutter_api_version),
  'Meta-@0@'.format(mutter_api_version),
  'Gcr-4',
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
  executable('gnome-shell-portal-helper',
    'gnome-shell-portal-helper.c', portal_resources,
    c_args: tools_cflags,
    dependencies: tools_deps,
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
