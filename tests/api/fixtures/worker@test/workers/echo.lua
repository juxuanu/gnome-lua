-- The worker of most cases of worker@test: echoes what it gets, and runs the
-- operation of a table with an `op` field (see the fixture)
local lib = require("lib")
local count = 0

return {
  on_message = function(v)
    if type(v) == "table" and v.op then
      if v.op == "progress" then
        shell.worker.post("p1")
        shell.worker.post("p2")
        return "done"
      elseif v.op == "log" then
        shell.worker.post("before the log")
        shell.log.warning("hello from the worker %d", 42)
        print("printed by the worker")
        shell.worker.post("after the log")
        return "logged"
      elseif v.op == "env" then
        return {
          timer = shell.timer == nil, fs = shell.fs == nil, settings = shell.settings == nil,
          hooks = shell.hooks == nil, dbus = shell.dbus == nil, spawn = shell.spawn == nil,
          network = shell.network == nil, ui = shell.ui == nil, panel = shell.panel == nil,
          test = shell.test == nil, start = shell.worker.start == nil, post = type(shell.worker.post),
          gettext = shell.gettext == nil and _ == nil,
          os = os == nil and io == nil and load == nil and debug == nil,
          uuid = shell.extension.uuid, dir = shell.extension.dir,
          name = shell.extension.metadata.name, caps = #shell.extension.capabilities,
          cap1 = shell.extension.capabilities[1], data_dir = shell.extension.data_dir == nil,
          lib = lib.twice(21), time = type(shell.time.monotonic()) == "number",
          minor = shell.api_version.minor,
          ro = not pcall(function() shell.extension.uuid = "x" end),
          gc = not pcall(setmetatable, {}, { __gc = function() end }),
        }
      elseif v.op == "slow" then
        local t = shell.time.monotonic()
        while shell.time.monotonic() - t < v.s do end
        return "slow-done"
      elseif v.op == "fail" then
        error("boom")
      elseif v.op == "bad" then
        return function() end
      elseif v.op == "nothing" then
        return nil
      elseif v.op == "count" then
        count = count + 1
        return count
      elseif v.op == "loop" then
        while true do end
      elseif v.op == "flood" then
        for i = 1, v.n do
          shell.worker.post(i)
        end
        return "flooded"
      elseif v.op == "big" then
        return string.rep("x", v.n)
      end
    end
    return v
  end,
}
