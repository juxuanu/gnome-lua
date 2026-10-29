-- Fixture of tests/api/network.lua: every function, field and error kind of
-- docs/api/network.md that can be tried without the internet. The CI has none:
-- the script of the test starts an HTTP server on 127.0.0.1 (test.http_server
-- of src/shell-test-runner.c) and gives its URL to the step "requests" below;
-- the script then looks at what the server received (the user agent, the
-- headers, the bodies). enable () runs the checks that are synchronous (the
-- arguments that raise) and reports T.done (); the requests are a step.
local shell = require("shell")
local T = shell.test
local net = shell.network

-- ---------------------------------------------------------------------
-- Helpers of the fixtures (tests/api/README.md): copy them as they are
-- ---------------------------------------------------------------------
local function case(name, fn, ...)
  local ok, err = pcall(fn, ...)
  T.report(name, ok, not ok and tostring(err) or nil)
  return ok
end

local function eq(got, want, what)
  if got ~= want then
    error(string.format("%s: expected %s, got %s", what or "value", tostring(want), tostring(got)), 2)
  end
end

local function truthy(value, what)
  if not value then
    error(string.format("%s: expected a true value, got %s", what or "value", tostring(value)), 2)
  end
  return value
end

-- fn () must raise an error that contains every string of `patterns` (a
-- string or a list; nil: any error)
local function raises(fn, patterns, what)
  local ok, err = pcall(fn)
  if ok then
    error(string.format("%s did not raise", what or "the call"), 2)
  end
  if type(patterns) == "string" then
    patterns = { patterns }
  end
  for _, pattern in ipairs(patterns or {}) do
    if not tostring(err):find(pattern, 1, true) then
      error(string.format("%s raised %q, expected it to mention %q", what or "the call", tostring(err), pattern), 2)
    end
  end
  return tostring(err)
end

-- handlers[name] (payload, done) runs when the script emits the step `name`;
-- it is finished when it returns, unless it returns "later": then it calls
-- done () itself
local function steps(handlers)
  shell.hooks.on("test.step", function(p)
    local handler = handlers[p.step]
    local finished = false

    if not handler then
      return
    end

    local function done()
      if not finished then
        finished = true
        T.done(p.step)
      end
    end

    local ok, result = pcall(handler, p, done)
    if not ok then
      T.report("step " .. p.step, false, tostring(result))
      done()
    elseif result ~= "later" then
      done()
    end
  end)
end

-- ---------------------------------------------------------------------
-- Asynchronous cases, one after the other (the connections per host are
-- limited, and a case that waits for a slow answer would hold up the rest):
-- each is { name, function (finish) ... end }; finish (true) or
-- finish (false, message) ends it, a case that never ends fails after
-- 10 s. guard (finish, fn) makes a callback that fails the case instead of
-- raising (an error of a callback would count against the extension).
-- ---------------------------------------------------------------------
local function guard(finish, fn)
  return function(...)
    local ok, err = pcall(fn, ...)
    if not ok then
      finish(false, err)
    end
  end
end

local function run_cases(list, all_done)
  local i = 0

  local function next_case()
    i = i + 1
    local c = list[i]
    local finished = false
    local watchdog

    if not c then
      all_done()
      return
    end

    local function finish(ok, message)
      if finished then
        return
      end
      finished = true
      if watchdog then
        watchdog:cancel()
      end
      T.report(c[1], ok, not ok and tostring(message) or nil)
      next_case()
    end

    local ok, err = pcall(c[2], finish)
    if not ok then
      finish(false, err)
    end
    if not finished then
      watchdog = shell.timer.after{ ms = 10000, callback = function()
        finish(false, "timed out")
      end }
    end
  end

  next_case()
end

-- a request whose answer must be a response: check (response) then the case ends
local function expect_response(finish, opts, check)
  opts.on_done = guard(finish, function(response)
    check(response)
    finish(true)
  end)
  opts.on_error = guard(finish, function(err)
    error(string.format("unexpected error %s: %s", tostring(err.code), tostring(err.message)))
  end)
  return net.request(opts)
end

-- a request whose answer must be the error `code`
local function expect_error(finish, opts, code)
  opts.on_done = function(response)
    finish(false, "expected the error " .. code .. ", got the status " .. tostring(response.status))
  end
  opts.on_error = guard(finish, function(err)
    eq(type(err), "table", "err")
    eq(err.code, code, "err.code")
    truthy(type(err.message) == "string" and err.message ~= "", "err.message is a string")
    finish(true)
  end)
  return net.request(opts)
end

local ext = {}

function ext.enable()
  -- -------------------------------------------------------------------
  -- The module, and every argument that raises
  -- -------------------------------------------------------------------
  case("shell.network.request exists", function()
    eq(type(net), "table", "shell.network")
    eq(type(net.request), "function", "shell.network.request")
  end)

  case("arguments that raise", function()
    raises(function() net.request() end, nil, "no argument")
    raises(function() net.request("http://x/") end, nil, "a string instead of a table")
    raises(function() net.request{} end, "url", "no url")
    raises(function() net.request{ url = 5 } end, "url", "a number as url")
    raises(function() net.request{ url = "http://x/", bogus = 1 } end, "bogus", "an unknown field")
    raises(function() net.request{ url = "ftp://x/" } end, "url", "ftp")
    raises(function() net.request{ url = "file:///etc/passwd" } end, "url", "file")
    raises(function() net.request{ url = "/relative" } end, "url", "a relative url")
    raises(function() net.request{ url = "http://" } end, "url", "no host")
    raises(function() net.request{ url = "http://user:pass@127.0.0.1:9/" } end, "url", "credentials in the url")
    raises(function() net.request{ url = "http://127.0.0.1:9/", method = "get" } end, "method", "a lower case method")
    raises(function() net.request{ url = "http://127.0.0.1:9/", method = "TRACE" } end, "method", "TRACE")
    raises(function() net.request{ url = "http://127.0.0.1:9/", timeout_ms = 0 } end, "timeout_ms", "timeout 0")
    raises(function() net.request{ url = "http://127.0.0.1:9/", timeout_ms = 1.5 } end, "timeout_ms", "a fraction")
    raises(function() net.request{ url = "http://127.0.0.1:9/", timeout_ms = "1" } end, "timeout_ms", "a string timeout")
    raises(function() net.request{ url = "http://127.0.0.1:9/", max_bytes = -1 } end, "max_bytes", "negative max_bytes")
    raises(function() net.request{ url = "http://127.0.0.1:9/", max_bytes = 64 * 1024 * 1024 + 1 } end, "max_bytes",
           "max_bytes over the cap")
    raises(function() net.request{ url = "http://127.0.0.1:9/", headers = "x" } end, "headers", "headers not a table")
    raises(function() net.request{ url = "http://127.0.0.1:9/", headers = { Host = "x" } } end, "Host",
           "a header the HTTP layer owns")
    raises(function() net.request{ url = "http://127.0.0.1:9/", headers = { ["Content-Length"] = "1" } } end,
           "Content-Length", "Content-Length")
    raises(function() net.request{ url = "http://127.0.0.1:9/", headers = { A = "1", a = "2" } } end, "twice",
           "a header given twice")
    raises(function() net.request{ url = "http://127.0.0.1:9/", headers = { A = 1 } } end, "string", "a number as value")
    raises(function() net.request{ url = "http://127.0.0.1:9/", headers = { ["A B"] = "1" } } end, "header name",
           "a bad header name")
    raises(function() net.request{ url = "http://127.0.0.1:9/", headers = { A = "1\r\nB: 2" } } end, "control",
           "a header value with a line break")
    raises(function() net.request{ url = "http://127.0.0.1:9/", body = "x" } end, "body", "a body on GET")
    raises(function() net.request{ url = "http://127.0.0.1:9/", method = "HEAD", body = "x" } end, "body",
           "a body on HEAD")
    raises(function() net.request{ url = "http://127.0.0.1:9/", method = "POST", body = 1 } end, "body",
           "a number as body")
    raises(function() net.request{ url = "http://127.0.0.1:9/", method = "POST", content_type = "text/plain" } end,
           "content_type", "content_type without a body")
    raises(function() net.request{ url = "http://127.0.0.1:9/", method = "POST", body = "x",
                                   content_type = "a/b", headers = { ["content-type"] = "c/d" } } end,
           "content_type", "content_type and a Content-Type header")
    raises(function() net.request{ url = "http://127.0.0.1:9/", on_done = 1 } end, "on_done", "on_done not a function")
    raises(function() net.request{ url = "http://127.0.0.1:9/", on_error = "x" } end, "on_error",
           "on_error not a function")
  end)

  -- -------------------------------------------------------------------
  -- The requests: the step "requests" of the script, {url, dead}
  -- -------------------------------------------------------------------
  steps{
    requests = function(p, done)
      local base, dead = p.url, p.dead
      local last_cancelled

      run_cases({
        { "GET: status, reason, body, headers (lower case), url", function(finish)
          local h
          h = expect_response(finish, { url = base .. "/hello", headers = { ["X-Mine"] = "yes" } }, function(r)
            eq(r.status, 200, "status")
            eq(r.reason, "OK", "reason")
            eq(r.body, "hello world", "body")
            eq(r.url, base .. "/hello", "url")
            eq(type(r.headers), "table", "headers")
            eq(r.headers["x-test"], "a", "headers['x-test']")
            truthy(r.headers["content-type"] and r.headers["content-type"]:find("text/plain", 1, true),
                   "content-type")
            eq(r.headers["Content-Type"], nil, "names are lower case")
            truthy(h.alive, "alive while the callback runs")
          end)
          eq(type(h), "userdata", "the handle")
          truthy(h.alive, "alive right after the call")
        end },

        { "a header sent more than once is joined with ', '", function(finish)
          expect_response(finish, { url = base .. "/multi" }, function(r)
            eq(r.headers["x-multi"], "one, two, three", "x-multi")
          end)
        end },

        { "an HTTP error status is a response: 404", function(finish)
          expect_response(finish, { url = base .. "/missing" }, function(r)
            eq(r.status, 404, "status")
            eq(r.body, "nope", "body")
          end)
        end },

        { "an HTTP error status is a response: 500", function(finish)
          expect_response(finish, { url = base .. "/boom" }, function(r)
            eq(r.status, 500, "status")
            eq(r.body, "server error", "body")
          end)
        end },

        { "an unknown path is a 404 response", function(finish)
          expect_response(finish, { url = base .. "/there-is-no-such-path" }, function(r)
            eq(r.status, 404, "status")
          end)
        end },

        { "POST echoes a binary body; content_type is sent", function(finish)
          expect_response(finish, { method = "POST", url = base .. "/echo", body = "a\0b\r\nc\255",
                                    content_type = "application/x-test" }, function(r)
            eq(r.status, 200, "status")
            eq(r.body, "a\0b\r\nc\255", "the echoed body")
            eq(#r.body, 7, "its length")
            eq(r.headers["x-echo-method"], "POST", "method")
          end)
        end },

        { "PUT without content_type is application/octet-stream", function(finish)
          expect_response(finish, { method = "PUT", url = base .. "/echo", body = "put body" }, function(r)
            eq(r.body, "put body", "body")
            eq(r.headers["x-echo-method"], "PUT", "method")
          end)
        end },

        { "PATCH with a Content-Type header", function(finish)
          expect_response(finish, { method = "PATCH", url = base .. "/echo", body = "{}",
                                    headers = { ["content-type"] = "application/json" } }, function(r)
            eq(r.body, "{}", "body")
            eq(r.headers["x-echo-method"], "PATCH", "method")
          end)
        end },

        { "DELETE", function(finish)
          expect_response(finish, { method = "DELETE", url = base .. "/echo" }, function(r)
            eq(r.headers["x-echo-method"], "DELETE", "method")
            eq(r.body, "", "body")
          end)
        end },

        { "HEAD: the headers of the GET and an empty body", function(finish)
          expect_response(finish, { method = "HEAD", url = base .. "/hello" }, function(r)
            eq(r.status, 200, "status")
            eq(r.body, "", "body")
            eq(r.headers["content-length"], "11", "content-length")
          end)
        end },

        { "HEAD is not refused for the size announced by its Content-Length", function(finish)
          expect_response(finish, { method = "HEAD", url = base .. "/big", max_bytes = 10 }, function(r)
            eq(r.status, 200, "status")
            eq(r.body, "", "body")
          end)
        end },

        { "204: an empty body", function(finish)
          expect_response(finish, { url = base .. "/empty" }, function(r)
            eq(r.status, 204, "status")
            eq(r.body, "", "body")
          end)
        end },

        { "a body with NUL bytes comes back whole", function(finish)
          expect_response(finish, { url = base .. "/nul" }, function(r)
            eq(r.body, "a\0b", "body")
          end)
        end },

        { "redirects are followed; url is the final one", function(finish)
          expect_response(finish, { url = base .. "/redir" }, function(r)
            eq(r.status, 200, "status")
            eq(r.body, "hello world", "body")
            eq(r.url, base .. "/hello", "url")
          end)
        end },

        { "more than 10 redirects: other", function(finish)
          expect_error(finish, { url = base .. "/loop" }, "other")
        end },

        { "connection refused: connection", function(finish)
          expect_error(finish, { url = dead .. "/x" }, "connection")
        end },

        { "timeout_ms: timeout", function(finish)
          expect_error(finish, { url = base .. "/slow", timeout_ms = 200 }, "timeout")
        end },

        { "a slow answer inside timeout_ms is a response", function(finish)
          expect_response(finish, { url = base .. "/slow", timeout_ms = 10000 }, function(r)
            eq(r.body, "late", "body")
          end)
        end },

        { "max_bytes: too-large when the length is announced", function(finish)
          expect_error(finish, { url = base .. "/big", max_bytes = 1000 }, "too-large")
        end },

        { "max_bytes: too-large by one byte", function(finish)
          expect_error(finish, { url = base .. "/big", max_bytes = 99999 }, "too-large")
        end },

        { "max_bytes: exactly the size is accepted", function(finish)
          expect_response(finish, { url = base .. "/big", max_bytes = 100000 }, function(r)
            eq(#r.body, 100000, "body length")
          end)
        end },

        { "max_bytes: too-large while the body is read (Content-Encoding hides the length)", function(finish)
          expect_error(finish, { url = base .. "/enc", max_bytes = 5000 }, "too-large")
        end },

        { "the default max_bytes (8 MiB) takes a body of 100000 bytes", function(finish)
          expect_response(finish, { url = base .. "/big" }, function(r)
            eq(#r.body, 100000, "body length")
          end)
        end },

        { "cancel (): on_error 'cancelled', from the main loop", function(finish)
          local in_cancel = true
          local h
          h = net.request{
            url = base .. "/slow",
            timeout_ms = 10000,
            on_done = function() finish(false, "a response arrived") end,
            on_error = guard(finish, function(err)
              eq(in_cancel, false, "on_error ran inside cancel ()")
              eq(err.code, "cancelled", "code")
              last_cancelled = h
              finish(true)
            end),
          }
          truthy(h.alive, "alive before cancel ()")
          h:cancel()
          h:cancel()   -- twice: nothing
          in_cancel = false
        end },

        { "a finished request is dead; cancel () and destroy () on it do nothing", function(finish)
          -- the previous case ended inside the callback: the request is over when it returns
          shell.timer.after{ ms = 20, callback = guard(finish, function()
            eq(last_cancelled.alive, false, "alive after the callback")
            last_cancelled:cancel()
            last_cancelled:destroy()
            finish(true)
          end) }
        end },

        { "destroy (): no callback runs", function(finish)
          local ran = false
          local h = net.request{
            url = base .. "/slow",
            on_done = function() ran = true end,
            on_error = function() ran = true end,
          }
          h:destroy()
          h:destroy()   -- idempotent
          eq(h.alive, false, "alive after destroy ()")
          h:cancel()    -- harmless on a dead handle
          shell.timer.after{ ms = 2000, callback = guard(finish, function()
            eq(ran, false, "a callback of a destroyed request ran")
            finish(true)
          end) }
        end },

        { "destroy () inside its own callback", function(finish)
          local h
          h = net.request{
            url = base .. "/hello",
            on_done = guard(finish, function()
              h:destroy()
              truthy(h.alive, "the handle stays valid until the callback returns")
              shell.timer.after{ ms = 20, callback = guard(finish, function()
                eq(h.alive, false, "alive after the callback")
                finish(true)
              end) }
            end),
            on_error = function(err) finish(false, err.code) end,
          }
        end },

        { "a request without callbacks, and a failing one without on_error", function(finish)
          net.request{ method = "POST", url = base .. "/echo", body = "fire and forget" }
          net.request{ url = dead .. "/x" }
          shell.timer.after{ ms = 300, callback = guard(finish, function()
            finish(true)
          end) }
        end },

        { "a request started from a callback", function(finish)
          net.request{
            url = base .. "/hello",
            on_done = guard(finish, function(outer)
              eq(outer.status, 200, "outer")
              net.request{
                url = base .. "/hello", method = "HEAD",
                on_done = guard(finish, function(inner)
                  eq(inner.status, 200, "inner")
                  finish(true)
                end),
                on_error = function(err) finish(false, err.code) end,
              }
            end),
            on_error = function(err) finish(false, err.code) end,
          }
        end },

        { "twenty requests at once", function(finish)
          local n = 0
          for i = 1, 20 do
            net.request{
              url = string.format("%s/hello?i=%d", base, i),
              on_done = guard(finish, function(r)
                eq(r.body, "hello world", "body")
                n = n + 1
                if n == 20 then
                  finish(true)
                end
              end),
              on_error = function(err) finish(false, err.code) end,
            }
          end
        end },
      }, done)

      return "later"
    end,

    -- a request in flight when the script disables the extension: its
    -- callbacks must never run (the script waits for the answer to be sent)
    inflight = function(p)
      for _, path in ipairs({ "/hold", "/hold" }) do
        net.request{
          url = p.url .. path,
          timeout_ms = 60000,
          on_done = function() T.report("a callback ran after disable (on_done)", false, "it ran") end,
          on_error = function() T.report("a callback ran after disable (on_error)", false, "it ran") end,
        }
      end
    end,
  }

  T.done()
end

return ext
