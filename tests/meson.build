mutter_tests_datadir = mutter_test_dep.get_variable('tests_datadir')
dbusrunnerconf = configuration_data()
dbusrunnerconf.set('MUTTER_TEST_PKGDATADIR', mutter_tests_datadir)
dbusrunnerconf.set('SRCDIR', meson.current_source_dir())

dbus_runner = configure_file(
  input: 'gnome-shell-dbus-runner.py.in',
  output: 'gnome-shell-dbus-runner.py',
  configuration: dbusrunnerconf,
)

# Shared tests dependencies. Nothing introspects any more: the shell tests
# run as Lua in the shell's own test runner
tests_dependencies = [
  compiled_schemas,
  data_resources_targets,
]

# GTest programs: they link libshell and drive the classes with an injected
# environment (clock, idle monitor, settings, logind, ...), so they need
# neither a session bus nor a display
c_unit_tests = {
  'break-manager': {},
  'extension-system': {},
  'file-utils': {
    # The data directories that the collection of files reads
    'env': {
      'XDG_DATA_DIRS': join_paths(meson.current_source_dir(), 'unit/fixtures/datadirs/system'),
      'XDG_DATA_HOME': join_paths(meson.current_source_dir(), 'unit/fixtures/datadirs/home'),
    },
  },
  'highlighter': {},
  'insert-sorted': {},
  'markup': {},
  'url': {},
  'version-compare': {},
  'time-limits-manager': {
    # The time limits tests hard-code UTC to keep the code brief, so must be
    # run in UTC
    'env': { 'TZ': 'UTC' },
  },
}

foreach test, extra_args : c_unit_tests
  c_test_exe = executable('test-@0@'.format(test), 'unit/test-@0@.c'.format(test),
    c_args: gnome_shell_cflags,
    dependencies: gnome_shell_deps + [libshell_dep, libst_dep, mutter_dep, json_glib_dep],
    include_directories: [conf_inc, include_directories('../src')],
    build_rpath: mutter_typelibdir,
  )

  c_test_env = environment()
  foreach var, value : extra_args.get('env', {})
    c_test_env.set(var, value)
  endforeach

  test(test, c_test_exe,
    args: ['--tap'],
    suite: 'unit',
    env: c_test_env,
    protocol: 'tap',
    workdir: meson.current_source_dir())
endforeach

shell_tests = [
  {
    'name': 'minimal',
  },
  {
    'name': 'basic',
  },
  {
    'name': 'overview',
  },
  {
    'name': 'closeWithActiveWindows',
    'env': {
      # The shader cache used by the clients can cause writes to the test home
      # directory during its removal by gnome-shell-test-tool
      'MESA_SHADER_CACHE_DISABLE': '1',
    },
  },
  {
    'name': 'multiMonitor',
  },
  {
    'name': 'uiGroupSize',
  },
  {
    'name': 'headlessStart',
    'options': ['--hotplug'],
  },
  {
    'name': 'fittsy',
  },
]

# Conformance tests of the extension API (docs/api, tests/api/README.md): one
# per area, `tests/api/<name>.lua`, run like the shell tests. 'fixtures' are
# the fixture extensions of tests/api/fixtures to install (default:
# '<name>@test'), 'extension_dirs' other extensions to install (absolute
# paths); the script enables them itself.
examples_dir = join_paths(meson.project_source_root(), 'subprojects', 'shell-lua', 'examples')

api_tests = [
  {
    'name': 'core',
    'fixtures': [
      'core@test',
      'core-bare@test',
      'core-errors@test',
      'core-minor@test',
      'core-future@test',
      'core-v2@test',
      'core-modes@test',
    ],
  },
  {
    'name': 'hooks',
    'fixtures': ['hooks@test', 'hooks-search@test'],
  },
  {
    'name': 'ui',
  },
  {
    'name': 'menu',
  },
  {
    'name': 'quick-settings',
  },
  {
    'name': 'notify',
  },
  {
    'name': 'window',
  },
  {
    'name': 'dbus',
    'fixtures': ['dbus@test', 'dbus-nocap@test'],
  },
  {
    'name': 'examples',
    'fixtures': [],
    'extension_dirs': [
      join_paths(examples_dir, 'hello@gnome-lua.example'),
      join_paths(examples_dir, 'indicator@gnome-lua.example'),
      join_paths(examples_dir, 'quick-toggle@gnome-lua.example'),
      join_paths(examples_dir, 'dash-launcher@gnome-lua.example'),
      join_paths(examples_dir, 'hooks-log@gnome-lua.example'),
      join_paths(examples_dir, 'prefs@gnome-lua.example'),
    ],
  },
]

libgvc_path = fs.parent(libgvc.get_variable('libgvc').full_path())
libshell_lua_path = fs.parent(libshell_lua.get_variable('libshell_lua').full_path())

background_file = join_paths(meson.current_source_dir(), 'data', 'background.png')

shell_testenv = environment()
shell_testenv.set('G_DEBUG', 'fatal-warnings')
shell_testenv.set('G_MESSAGES_DEBUG', 'GNOME Shell')
shell_testenv.set('GNOME_SHELL_DATADIR', data_builddir)
shell_testenv.set('GNOME_SHELL_BUILDDIR', src_builddir)
shell_testenv.set('GNOME_SHELL_SESSION_MODE', 'user')
shell_testenv.set('SHELL_BACKGROUND_IMAGE', '@0@'.format(background_file))
shell_testenv.append('LD_LIBRARY_PATH', libgvc_path, separator: ':')
shell_testenv.append('LD_LIBRARY_PATH', libshell_lua_path, separator: ':')

foreach shell_test : shell_tests
  test_name = shell_test['name']
  options = shell_test.get('options', [])

  local_test_env = shell_testenv
  foreach var, value : shell_test.get('env', {})
    local_test_env.set(var, value)
  endforeach

  test(test_name, dbus_runner,
    suite: 'shell',
    args: [
      test_tool,
      '--headless',
      options,
      '@0@/shell/@1@.lua'.format(meson.current_source_dir(), test_name),
    ],
    depends: [
      gnome_shell_binary,
      gnome_shell_perf_helper,
      tests_dependencies,
    ],
    env: local_test_env,
  )
endforeach

# The conformance tests of the API: the same shell test, started with the
# fixture extensions installed in the test home (not enabled)
foreach api_test : api_tests
  test_name = api_test['name']
  options = api_test.get('options', [])
  extension_args = []

  foreach fixture : api_test.get('fixtures', ['@0@'.format(test_name) + '@test'])
    extension_args += [
      '--install-extension',
      join_paths(meson.current_source_dir(), 'api', 'fixtures', fixture),
    ]
  endforeach

  foreach extension_dir : api_test.get('extension_dirs', [])
    extension_args += ['--install-extension', extension_dir]
  endforeach

  test('api-@0@'.format(test_name), dbus_runner,
    suite: 'api',
    args: [
      test_tool,
      '--headless',
      options,
      extension_args,
      '@0@/api/@1@.lua'.format(meson.current_source_dir(), test_name),
    ],
    depends: [
      gnome_shell_binary,
      gnome_shell_perf_helper,
      tests_dependencies,
    ],
    env: shell_testenv,
    timeout: 120,
  )
endforeach
