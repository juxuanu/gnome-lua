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
  'clock@gnome-lua.example/metadata.json',
  'clock@gnome-lua.example/extension.lua',
  'clock@gnome-lua.example/stylesheet.css',
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
  'power-profile@gnome-lua.example/metadata.json',
  'power-profile@gnome-lua.example/extension.lua',
  'text-scale@gnome-lua.example/metadata.json',
  'text-scale@gnome-lua.example/extension.lua',
  'emoji-search@gnome-lua.example/metadata.json',
  'emoji-search@gnome-lua.example/extension.lua',
  'load-average@gnome-lua.example/metadata.json',
  'load-average@gnome-lua.example/extension.lua',
  'disk-space@gnome-lua.example/metadata.json',
  'disk-space@gnome-lua.example/extension.lua',
  'weather@gnome-lua.example/metadata.json',
  'weather@gnome-lua.example/extension.lua',
  'pi-digits@gnome-lua.example/metadata.json',
  'pi-digits@gnome-lua.example/extension.lua',
  'pi-digits@gnome-lua.example/workers/pi.lua',
]
  if fs.exists(f)
    shell_lua_examples += files(f)
  endif
endforeach
