# Skeleton: another agent fills in shell_lua_tests. Each name N is built from
# test-N.c against libshell_lua_dep and run as test 'N'. lua_exe (the vendored
# lua interpreter) is available for tests that need to spawn it.
shell_lua_tests = []

test_env = environment()
test_env.set('G_TEST_SRCDIR', meson.current_source_dir())
test_env.set('G_TEST_BUILDDIR', meson.current_build_dir())
test_env.set('GSETTINGS_BACKEND', 'memory')
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
  )
endforeach
