# Examples are documentation only: listed here so they are part of the
# project's file set, never installed. examples/hello@gnome-lua.example is the
# reference extension; tests/api/examples.lua enables each of them in a
# headless shell.
fs = import('fs')

shell_lua_examples = []
foreach f : [
  'hello@gnome-lua.example/metadata.json',
  'hello@gnome-lua.example/extension.lua',
  'hello@gnome-lua.example/prefs.lua',
  'hello@gnome-lua.example/schemas/org.gnome-lua.example.hello.gschema.xml',
  'indicator@gnome-lua.example/metadata.json',
  'indicator@gnome-lua.example/extension.lua',
  'indicator@gnome-lua.example/stylesheet.css',
  'quick-toggle@gnome-lua.example/metadata.json',
  'quick-toggle@gnome-lua.example/extension.lua',
  'quick-toggle@gnome-lua.example/schemas/org.gnome-lua.example.quick-toggle.gschema.xml',
  'dash-launcher@gnome-lua.example/metadata.json',
  'dash-launcher@gnome-lua.example/extension.lua',
  'hooks-log@gnome-lua.example/metadata.json',
  'hooks-log@gnome-lua.example/extension.lua',
  'prefs@gnome-lua.example/metadata.json',
  'prefs@gnome-lua.example/extension.lua',
  'prefs@gnome-lua.example/prefs.lua',
  'prefs@gnome-lua.example/schemas/org.gnome-lua.example.prefs.gschema.xml',
]
  if fs.exists(f)
    shell_lua_examples += files(f)
  endif
endforeach
