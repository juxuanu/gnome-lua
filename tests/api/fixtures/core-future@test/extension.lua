-- Fixture of tests/api/core.lua: only its metadata.json matters.
local shell = require("shell")
local T = shell.test

return {
  enable = function()
    -- reached only when the loader accepts the metadata
    T.report("enabled", true)
    T.done()
  end,
}
