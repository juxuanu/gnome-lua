-- A pure-Lua library shared by the extension and its workers (docs/api/worker.md
-- "Inside a worker": the same require roots)
return { twice = function(n) return 2 * n end }
