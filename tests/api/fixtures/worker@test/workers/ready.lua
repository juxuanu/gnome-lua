-- Posts while it loads, then echoes
shell.worker.post("ready")
return { on_message = function(v) return v end }
