# Each name N is built from test-N.c against libshell_lua_dep and run as test
# 'N' (TAP). test-utils.h holds the small helpers shared by the programs.
# lua_exe (the vendored lua interpreter) is available for tests that need to
# spawn it.
shell_lua_tests = [
  'state',
  'variant',
  'hooks',
  'handle',
  'api-core',
  'api-dbus',
  'api-fs',
  'api-spawn',
  'api-time',
  'objects',
  'extension',
  'inspector',
  'fuzz',
]

# The test schemas (org.gnome-lua.test, a minimal org.gnome.shell) are
# compiled into this directory; GSETTINGS_SCHEMA_DIR makes GSettings see them.
glib_compile_schemas = find_program('glib-compile-schemas')
test_gschemas = custom_target('test-gschemas',
  input: [
    'schemas/org.gnome-lua.test.gschema.xml',
    'schemas/org.gnome.shell.gschema.xml',
  ],
  output: 'gschemas.compiled',
  command: [
    glib_compile_schemas, '--strict',
    '--targetdir=@OUTDIR@',
    meson.current_source_dir() / 'schemas',
  ],
  build_by_default: true,
)

test_env = environment()
test_env.set('G_TEST_SRCDIR', meson.current_source_dir())
test_env.set('G_TEST_BUILDDIR', meson.current_build_dir())
test_env.set('GSETTINGS_BACKEND', 'memory')
test_env.set('GSETTINGS_SCHEMA_DIR', meson.current_build_dir())
test_env.set('G_DEBUG', 'fatal-warnings')

foreach name : shell_lua_tests
  test_exe = executable('test-' + name,
    'test-@0@.c'.format(name),
    dependencies: [libshell_lua_dep],
    include_directories: shell_lua_inc,
    c_args: ['-UG_DISABLE_ASSERT'],
  )

  test(name, test_exe,
    env: test_env,
    protocol: 'tap',
    depends: test_gschemas,
    timeout: 90,
  )
endforeach
