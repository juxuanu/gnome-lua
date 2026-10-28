project('shell-lua', 'c',
  version: '1.0.0',
  license: 'GPL-2.0-or-later',
  meson_version: '>= 1.3.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
  ]
)

cc = meson.get_compiler('c')
gnome = import('gnome')

pkglibdir = get_option('pkglibdir')
pkgdatadir = get_option('pkgdatadir')

enable_static = get_option('static')
introspection = get_option('introspection')
if enable_static
  # g-ir-scanner needs a shared library to dump.
  if introspection.enabled()
    error('introspection=enabled requires static=false')
  endif
  introspection = introspection.disable_auto_if(true)
endif
introspection = introspection.require(
  find_program('g-ir-scanner', required: false).found(),
  error_message: 'g-ir-scanner not found'
)
enable_introspection = introspection.allowed()

if not enable_static
  assert(pkglibdir != '', 'Installing shared library, but pkglibdir is unset!')
endif

# The same warning set gnome-shell's root meson.build uses, applied to this
# project only (add_project_arguments does not cross subproject boundaries).
if get_option('buildtype') != 'plain'
  shell_lua_warn_args = [
    '-fno-strict-aliasing',
    '-Wpointer-arith',
    '-Wmissing-declarations',
    '-Wimplicit-function-declaration',
    '-Wformat=2',
    '-Wformat-nonliteral',
    '-Wformat-security',
    '-Wstrict-prototypes',
    '-Wmissing-prototypes',
    '-Wnested-externs',
    '-Wold-style-definition',
    '-Wundef',
    '-Wunused',
    '-Wcast-align',
    '-Wmissing-noreturn',
    '-Wmissing-format-attribute',
    '-Wmissing-include-dirs',
    '-Wlogical-op',
    '-Wignored-qualifiers',
    '-Werror=redundant-decls',
    '-Werror=implicit',
    '-Werror=nonnull',
    '-Werror=init-self',
    '-Werror=main',
    '-Werror=missing-braces',
    '-Werror=sequence-point',
    '-Werror=return-type',
    '-Werror=trigraphs',
    '-Werror=array-bounds',
    '-Werror=write-strings',
    '-Werror=address',
    '-Werror=int-to-pointer-cast',
    '-Werror=pointer-to-int-cast',
    '-Werror=empty-body',
  ]

  if get_option('debug')
    shell_lua_warn_args += '-DG_ENABLE_DEBUG'
  else
    shell_lua_warn_args += [
      '-DG_DISABLE_ASSERT',
      '-DG_DISABLE_CAST_CHECKS',
    ]
  endif

  add_project_arguments(cc.get_supported_arguments(shell_lua_warn_args),
                        language: 'c')
endif

add_project_arguments(
  '-D_GNU_SOURCE',
  '-DSHELL_LUA_VERSION="@0@"'.format(meson.project_version()),
  language: 'c'
)

glib_dep = dependency('glib-2.0', version: '>= 2.80')
gobject_dep = dependency('gobject-2.0', version: '>= 2.80')
gio_dep = dependency('gio-2.0', version: '>= 2.80')
gio_unix_dep = dependency('gio-unix-2.0', version: '>= 2.80')
json_glib_dep = dependency('json-glib-1.0')
m_dep = cc.find_library('m', required: false)

# Always the vendored Lua (subprojects/lua.wrap); never a system Lua.
# The interpreter is only needed by the tests.
lua_proj = subproject('lua',
  default_options: ['interpreter=@0@'.format(get_option('tests'))]
)
lua_dep = lua_proj.get_variable('lua_dep')
lua_exe = lua_proj.get_variable('lua_exe', disabler())

subdir('src')

if get_option('tests')
  subdir('tests')
endif

subdir('examples')
