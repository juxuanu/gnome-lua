# Examples are documentation only: listed here so they are part of the
# project's file set, never installed. examples/hello@gnome-lua.example is the reference extension.
fs = import('fs')

shell_lua_examples = []
foreach f : ['hello@gnome-lua.example/metadata.json', 'hello@gnome-lua.example/extension.lua']
  if fs.exists(f)
    shell_lua_examples += files(f)
  endif
endforeach
