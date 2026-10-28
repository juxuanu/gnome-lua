# org.gnome.Shell.Extensions: forwards to the extension manager of the shell and
# shows the window of the preferences of an extension (docs/api/prefs.md). It
# runs outside of the compositor and is not linked with libshell, but with
# libshell-lua, which runs the sandboxed prefs.lua of the extension.
extensions_service_dbus = gnome.gdbus_codegen('extensions-service-dbus',
  sources: files('../../../data/dbus-interfaces/org.gnome.Shell.Extensions.xml'),
  interface_prefix: 'org.gnome.Shell.',
  namespace: 'ShellDBus',
  autocleanup: 'all'
)

extension_prefs_schema_sources = files(
  'extension-prefs-schema.c',
  'extension-prefs-schema.h',
)

extensions_service_deps = [
  gio_dep,
  gtk_dep,
  libadwaita_dep,
  json_glib_dep,
  libshell_lua_dep,
  libshew,
  m_dep,
]

extensions_service_includes = [
  conf_inc,
  include_directories('.', '..'),
]

# The executable has the name of the launcher that js/dbusServices installed
executable('org.gnome.Shell.Extensions',
  [
    dbus_service_base,
    extension_prefs_schema_sources,
    'extension-prefs-window.c',
    'extension-prefs-window.h',
    'extensions-service.c',
    'extensions-service.h',
    'main.c',
    extensions_service_dbus,
  ],
  dependencies: extensions_service_deps,
  include_directories: extensions_service_includes,
  c_args: [
    '-DG_LOG_DOMAIN="org.gnome.Shell.Extensions"',
    '-DLOCALEDIR="@0@"'.format(localedir),
  ],
  install_rpath: pkglibdir,
  install_dir: libexecdir,
  install: true
)

configure_file(
  input: 'org.gnome.Shell.Extensions.service.in',
  output: 'org.gnome.Shell.Extensions.service',
  configuration: service_data,
  install_dir: servicedir
)

if get_option('tests')
  # The validation of prefs.lua needs neither GTK nor a display
  test_extension_prefs_schema = executable('test-extension-prefs-schema',
    [
      'tests/test-extension-prefs-schema.c',
      extension_prefs_schema_sources,
    ],
    dependencies: [gio_dep, json_glib_dep, libshell_lua_dep, m_dep],
    include_directories: extensions_service_includes,
    c_args: '-UG_DISABLE_ASSERT',
  )

  test('extension-prefs-schema', test_extension_prefs_schema,
    args: ['--tap'],
    suite: 'unit',
    env: {
      'HELLO_EXTENSION_DIR': meson.project_source_root() / 'subprojects' / 'shell-lua' /
        'examples' / 'hello@gnome-lua.example',
    },
    protocol: 'tap')
endif
