-- workers/pi.lua: the worker of pi-digits@gnome-lua.example. It runs in a
-- thread of its own (docs/api/worker.md): no shell objects exist here, only
-- shell.log, shell.time, a copy of shell.extension and shell.worker.post.
--
-- The digits come from the spigot of Rabinowitz and Wagon, which needs
-- nothing but integers and takes time proportional to the square of the
-- number of digits: a few thousand digits keep a thread busy for seconds,
-- which is the point of a worker. On the main thread the same loop would
-- freeze the desktop until the watchdog stops it.

local function pi_digits(n, progress)
  local len = 10 * n // 3 + 1
  local a = {}
  local out = {}
  local nines, predigit = 0, 0

  for i = 1, len do
    a[i] = 2
  end

  for j = 1, n do
    local q = 0

    for i = len, 1, -1 do
      local x = 10 * a[i] + q * i
      a[i] = x % (2 * i - 1)
      q = x // (2 * i - 1)
    end
    a[1] = q % 10
    q = q // 10

    if q == 9 then
      nines = nines + 1
    elseif q == 10 then
      out[#out + 1] = tostring(predigit + 1)
      out[#out + 1] = string.rep("0", nines)
      predigit, nines = 0, 0
    else
      out[#out + 1] = tostring(predigit)
      predigit = q
      out[#out + 1] = string.rep("9", nines)
      nines = 0
    end

    progress(j, n)
  end
  out[#out + 1] = tostring(predigit)

  -- the first digit out is a 0 that only primes the predigit
  local digits = table.concat(out):sub(2)
  return digits:sub(1, 1) .. "." .. digits:sub(2, n)
end

return {
  -- One message: the number of digits. Progress goes out with
  -- shell.worker.post as it comes; the return value is the answer.
  on_message = function(n)
    local last = -1
    local start = shell.time.monotonic()
    -- a few more than asked: the spigot can leave its last digits unsettled
    local digits = pi_digits(n + 5, function(j, total)
      local percent = j * 100 // total

      if percent ~= last and percent % 5 == 0 then
        last = percent
        shell.worker.post{ progress = percent }
      end
    end)

    shell.log.debug("%d digits of pi in %.1f s", n, shell.time.monotonic() - start)
    return { digits = digits:sub(1, n + 1) }   -- "3." and n - 1 decimals
  end,
}
