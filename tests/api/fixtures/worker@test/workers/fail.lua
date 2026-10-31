-- Raises on its first message
return { on_message = function(v) error("the worker failed on " .. tostring(v)) end }
