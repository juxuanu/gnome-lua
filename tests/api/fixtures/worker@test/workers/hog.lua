-- Grows past max_memory
return {
  on_message = function()
    local t = {}
    while true do
      t[#t + 1] = string.rep("x", 4096)
    end
  end,
}
