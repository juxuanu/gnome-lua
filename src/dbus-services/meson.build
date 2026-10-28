# The out-of-process D-Bus services of the shell. They are linked with GLib and
# GIO (and GStreamer, GTK and JSON-GLib for the screencast service) only: they
# run outside of the compositor and do not use libshell.
gst_dep = dependency('gstreamer-1.0')

dbus_service_includes = include_directories('../..')

dbus_service_base = files(
  'shell-dbus-service.c',
  'shell-dbus-service.h',
)

dbus_interfaces_dir = '../../data/dbus-interfaces'

# The skeleton and the proxy of org.gnome.ScreenSaver (type ShellDBusScreenSaver)
screensaver_service_dbus = gnome.gdbus_codegen('screensaver-service-dbus',
  sources: dbus_interfaces_dir / 'org.gnome.ScreenSaver.xml',
  interface_prefix: 'org.gnome.',
  namespace: 'ShellDBus'
)

# The skeleton and the proxy of org.freedesktop.Notifications (type
# ShellFreedesktopNotifications)
notification_service_dbus = gnome.gdbus_codegen('notification-service-dbus',
  sources: dbus_interfaces_dir / 'org.freedesktop.Notifications.xml',
  interface_prefix: 'org.',
  namespace: 'Shell'
)

# The skeleton of org.gnome.Shell.Screencast (type ShellDBusScreencast)
screencast_service_dbus = gnome.gdbus_codegen('screencast-service-dbus',
  sources: dbus_interfaces_dir / 'org.gnome.Shell.Screencast.xml',
  interface_prefix: 'org.gnome.Shell.',
  namespace: 'ShellDBus'
)

# The proxy of org.gnome.Shell.Introspect (type ShellIntrospect)
introspect_service_dbus = gnome.gdbus_codegen('introspect-service-dbus',
  sources: dbus_interfaces_dir / 'org.gnome.Shell.Introspect.xml',
  interface_prefix: 'org.gnome.Shell.',
  namespace: 'Shell'
)

# The proxies of org.gnome.Mutter.ScreenCast (types ShellMutterScreenCast,
# ShellMutterScreenCastSession and ShellMutterScreenCastStream)
mutter_screencast_dbus = gnome.gdbus_codegen('mutter-screencast-dbus',
  sources: dbus_interfaces_dir / 'org.gnome.Mutter.ScreenCast.xml',
  interface_prefix: 'org.gnome.Mutter.',
  namespace: 'ShellMutter'
)

# The executables have the names of the launchers that js/dbusServices used to
# install in the package data directory
executable('org.gnome.ScreenSaver',
  [dbus_service_base, 'screensaver-service.c', screensaver_service_dbus],
  dependencies: [gio_dep],
  include_directories: dbus_service_includes,
  c_args: '-DG_LOG_DOMAIN="org.gnome.ScreenSaver"',
  install_dir: libexecdir,
  install: true
)

executable('org.gnome.Shell.Notifications',
  [dbus_service_base, 'notification-service.c', notification_service_dbus],
  dependencies: [gio_dep],
  include_directories: dbus_service_includes,
  c_args: '-DG_LOG_DOMAIN="org.gnome.Shell.Notifications"',
  install_dir: libexecdir,
  install: true
)

executable('org.gnome.Shell.Screencast',
  [
    dbus_service_base,
    'screencast-service.c',
    screencast_service_dbus,
    introspect_service_dbus,
    mutter_screencast_dbus,
  ],
  dependencies: [gio_dep, gst_dep, gtk_dep, json_glib_dep],
  include_directories: dbus_service_includes,
  c_args: '-DG_LOG_DOMAIN="org.gnome.Shell.Screencast"',
  install_dir: libexecdir,
  install: true
)

foreach service_file : [
  'org.gnome.ScreenSaver.service',
  'org.gnome.Shell.Notifications.service',
  'org.gnome.Shell.Screencast.service',
]
  configure_file(
    input: service_file + '.in',
    output: service_file,
    configuration: service_data,
    install_dir: servicedir
  )
endforeach
