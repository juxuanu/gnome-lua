-- Fixture of tests/api/arguments.lua: the message of an error can hold any
-- bytes, and the shell sends it over D-Bus as a string (ListExtensions,
-- GetExtensionErrors, ExtensionStatusChanged).
local ext = {}

function ext.enable()
  error("bad \xC3 byte", 0)
end

function ext.disable()
end

return ext
