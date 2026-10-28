shell_lua_public_headers = [
  'shell-lua.h',
  'shell-hooks.h',
  'shell-lua-handle.h',
  'shell-lua-variant.h',
  'shell-lua-extension.h',
  'shell-lua-inspector.h',
]

shell_lua_sources = [
  'shell-lua.c',
  'shell-lua-api.c',
  'shell-lua-api-core.c',
  'shell-lua-api-hooks.c',
  'shell-lua-api-dbus.c',
  'shell-lua-objects.c',
  'shell-lua-handle.c',
  'shell-lua-variant.c',
  'shell-hooks.c',
  'shell-lua-extension.c',
  'shell-lua-extension-manager.c',
  'shell-lua-inspector.c',
  'shell-lua-connection.c',
]

# Not scanned for GIR.
shell_lua_private_headers = [
  'shell-lua-api-private.h',
]

shell_lua_deps = [
  glib_dep,
  gobject_dep,
  gio_dep,
  gio_unix_dep,
  json_glib_dep,
  lua_dep,
  m_dep,
]

shell_lua_inc = include_directories('.')

if enable_static
  libshell_lua = static_library('shell-lua',
    sources: shell_lua_sources + shell_lua_public_headers + shell_lua_private_headers,
    dependencies: shell_lua_deps,
    include_directories: shell_lua_inc,
    pic: true
  )
else
  libshell_lua = shared_library('shell-lua',
    sources: shell_lua_sources + shell_lua_public_headers + shell_lua_private_headers,
    dependencies: shell_lua_deps,
    include_directories: shell_lua_inc,
    install_dir: pkglibdir,
    install: true
  )
endif

if enable_introspection
  assert(pkgdatadir != '', 'Installing introspection, but pkgdatadir is unset!')

  # The handle and variant layers take lua_State* and are not introspectable;
  # only the GObject-facing headers go through the scanner.
  shell_lua_gir_sources = files(
    'shell-lua.c', 'shell-lua.h',
    'shell-hooks.c', 'shell-hooks.h',
    'shell-lua-extension.c', 'shell-lua-extension-manager.c', 'shell-lua-extension.h',
    'shell-lua-inspector.c', 'shell-lua-inspector.h',
  )

  libshell_lua_gir = gnome.generate_gir(libshell_lua,
    sources: shell_lua_gir_sources,
    fatal_warnings: true,
    nsversion: '1',
    namespace: 'ShellLua',
    identifier_prefix: 'Shell',
    symbol_prefix: 'shell',
    includes: ['GObject-2.0', 'Gio-2.0', 'Json-1.0'],
    dependencies: shell_lua_deps,
    extra_args: ['--quiet'],
    install_dir_gir: pkgdatadir,
    install_dir_typelib: pkglibdir,
    install: true
  )
endif

libshell_lua_dep = declare_dependency(
  link_with: libshell_lua,
  include_directories: shell_lua_inc,
  dependencies: shell_lua_deps
)
