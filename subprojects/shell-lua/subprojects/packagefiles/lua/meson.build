# Meson build for the vendored Lua 5.5.1 (overlay for subprojects/lua.wrap).
# Mirrors the upstream src/Makefile "linux" target: -DLUA_USE_LINUX
# (which implies LUA_USE_POSIX and LUA_USE_DLOPEN), -lm -ldl, and -Wl,-E for
# the interpreter.
project('lua', 'c',
  version: '5.5.1',
  license: 'MIT',
  meson_version: '>= 1.3.0',
  default_options: [
    'c_std=gnu99',
    'warning_level=1',
  ]
)

cc = meson.get_compiler('c')

lua_c_args = []
if host_machine.system() == 'linux'
  lua_c_args += '-DLUA_USE_LINUX'
elif host_machine.system() == 'darwin'
  lua_c_args += '-DLUA_USE_MACOSX'
elif host_machine.system() != 'windows'
  lua_c_args += '-DLUA_USE_POSIX'
endif

lua_link_deps = [cc.find_library('m', required: false)]
if host_machine.system() == 'linux'
  # dlopen() lives in libdl on older glibc; harmless no-op stub on newer.
  lua_link_deps += cc.find_library('dl', required: false)
endif

lua_core_sources = files(
  'src/lapi.c',
  'src/lauxlib.c',
  'src/lbaselib.c',
  'src/lcode.c',
  'src/lcorolib.c',
  'src/lctype.c',
  'src/ldblib.c',
  'src/ldebug.c',
  'src/ldo.c',
  'src/ldump.c',
  'src/lfunc.c',
  'src/lgc.c',
  'src/linit.c',
  'src/liolib.c',
  'src/llex.c',
  'src/lmathlib.c',
  'src/lmem.c',
  'src/loadlib.c',
  'src/lobject.c',
  'src/lopcodes.c',
  'src/loslib.c',
  'src/lparser.c',
  'src/lstate.c',
  'src/lstring.c',
  'src/lstrlib.c',
  'src/ltable.c',
  'src/ltablib.c',
  'src/ltm.c',
  'src/lundump.c',
  'src/lutf8lib.c',
  'src/lvm.c',
  'src/lzio.c',
)

lua_inc = include_directories('src', is_system: true)

liblua = static_library('lua',
  lua_core_sources,
  c_args: lua_c_args,
  dependencies: lua_link_deps,
  include_directories: lua_inc,
  pic: true,
  install: false
)

lua_dep = declare_dependency(
  link_with: liblua,
  include_directories: lua_inc,
  dependencies: lua_link_deps,
  version: meson.project_version()
)

if get_option('interpreter')
  lua_exe = executable('lua',
    'src/lua.c',
    c_args: lua_c_args,
    dependencies: lua_dep,
    export_dynamic: true,
    install: false
  )
endif
