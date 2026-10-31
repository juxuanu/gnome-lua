-- Never returns from its first message; stopped by terminate (), destroy ()
-- and disable, also when it catches its errors
return {
  on_message = function(v)
    if v == "catch" then
      while true do pcall(function() while true do end end) end
    end
    while true do end
  end,
}
