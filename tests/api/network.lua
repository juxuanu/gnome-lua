-- Conformance test of docs/api/network.md (API 1.1): see tests/api/README.md.
-- The assertions of the API are in the fixtures tests/api/fixtures/network@test
-- (the arguments that raise, and every request against a server of this
-- script) and network-nocap@test (nothing works without the capability), run
-- by the extension manager of the shell. The CI has no internet: this script
-- starts an HTTP server on 127.0.0.1 (test.http_server, src/shell-test-runner.c)
-- and gives the fixture its URL with a step; afterwards it looks at what the
-- server received (user agent, headers, bodies, the redirects) and at what
-- happens to a request that is in flight when the extension is disabled.

local harness = dofile(((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local uuid = "network@test"

local function fail (fmt, ...)
  error (string.format (fmt, ...), 0)
end

test.wait_startup ()

-- Without "network" in the capabilities shell.network.request raises
harness.run { uuid = "network-nocap@test" }

local server = test.http_server {
  routes = {
    ["/hello"] = { body = "hello world", headers = { ["X-Test"] = "a" } },
    ["/multi"] = { body = "m", headers = { ["X-Multi"] = { "one", "two", "three" } } },
    ["/echo"] = { echo = true },
    ["/missing"] = { status = 404, body = "nope" },
    ["/boom"] = { status = 500, body = "server error" },
    ["/empty"] = { status = 204 },
    ["/nul"] = { body = "a\0b" },
    ["/slow"] = { body = "late", delay_ms = 1500 },
    ["/hold"] = { body = "late", delay_ms = 1500 },
    ["/big"] = { size = 100000 },
    -- a body whose length the client cannot read from the headers
    ["/enc"] = { size = 100000, headers = { ["Content-Encoding"] = "x-nothing" } },
    ["/redir"] = { redirect = "/hello" },
    ["/loop"] = { redirect = "/loop" },
  },
}

-- a port where nothing listens: a server that was stopped
local dead = test.http_server {}
dead:stop ()

-- what the server received since the request number `from`
local function received (from)
  local seen, list = server:requests (), {}

  for i = from + 1, #seen do
    list[#list + 1] = seen[i]
  end
  return list
end

local function find (list, method, path)
  for _, q in ipairs (list) do
    if q.method == method and q.path == path then
      return q
    end
  end
end

local function count (list, method, path)
  local n = 0

  for _, q in ipairs (list) do
    if q.method == method and q.path == path then
      n = n + 1
    end
  end
  return n
end

local function check_received (list)
  local q = find (list, "GET", "/hello")

  if not q then
    fail ("the server saw no GET /hello")
  end
  if q.headers["user-agent"] == nil or q.headers["user-agent"] ~= "gnome-lua/" .. uuid then
    fail ("the user agent is %q, expected %q", tostring (q.headers["user-agent"]), "gnome-lua/" .. uuid)
  end

  -- the request with the header X-Mine (the first case of the fixture)
  local mine
  for _, r in ipairs (list) do
    if r.method == "GET" and r.path == "/hello" and r.headers["x-mine"] then
      mine = r
    end
  end
  if not mine then
    fail ("no request carried the header X-Mine")
  end
  if mine.headers["x-mine"] ~= "yes" then
    fail ("X-Mine is %q", tostring (mine.headers["x-mine"]))
  end
  if mine.headers["accept"] ~= "*/*" then
    fail ("Accept is %q, expected */*", tostring (mine.headers["accept"]))
  end

  for _, r in ipairs (list) do
    if r.headers["cookie"] then
      fail ("a request carried a cookie: %s", r.headers["cookie"])
    end
    if r.headers["authorization"] then
      fail ("a request carried an Authorization header")
    end
  end

  local post
  for _, r in ipairs (list) do
    if r.method == "POST" and r.path == "/echo" and r.body ~= "fire and forget" then
      post = r
    end
  end
  if not post then
    fail ("the server saw no POST /echo")
  end
  if post.body ~= "a\0b\r\nc\255" then
    fail ("the body of the POST is %q", post.body)
  end
  if post.headers["content-type"] ~= "application/x-test" then
    fail ("the POST has the content type %q", tostring (post.headers["content-type"]))
  end

  local put = find (list, "PUT", "/echo")
  if not put or put.headers["content-type"] ~= "application/octet-stream" then
    fail ("the PUT has the content type %q, expected application/octet-stream",
          put and tostring (put.headers["content-type"]) or "(no PUT)")
  end

  local patch = find (list, "PATCH", "/echo")
  if not patch or patch.headers["content-type"] ~= "application/json" or patch.body ~= "{}" then
    fail ("the PATCH is not what was asked for")
  end

  if not find (list, "DELETE", "/echo") then
    fail ("the server saw no DELETE")
  end
  if not find (list, "HEAD", "/hello") then
    fail ("the server saw no HEAD")
  end

  local ff = 0
  for _, r in ipairs (list) do
    if r.method == "POST" and r.body == "fire and forget" then
      ff = ff + 1
    end
  end
  if ff ~= 1 then
    fail ("the request without callbacks reached the server %d times", ff)
  end

  -- /redir was followed to /hello (the get case, and this one)
  if count (list, "GET", "/redir") ~= 1 then
    fail ("GET /redir was sent %d times", count (list, "GET", "/redir"))
  end

  -- a redirect loop is given up after 10 redirects
  local loops = count (list, "GET", "/loop")
  if loops < 10 or loops > 13 then
    fail ("GET /loop was sent %d times, expected about 11", loops)
  end

  if count (list, "GET", "/hello?i=1") ~= 1 or count (list, "GET", "/hello?i=20") ~= 1 then
    fail ("the twenty parallel requests did not all reach the server")
  end
end

harness.run {
  uuid = uuid,
  timeout = 60000,

  drive = function (c)
    local from = #server:requests ()

    c.wait_done ("main")
    c.step ("requests", { url = server.url, dead = dead.url })
    check_received (received (from))

    -- two requests the server holds back for 1.5 s: the extension is disabled
    -- while they are in flight (the harness does it right after this)
    c.step ("inflight", { url = server.url })
  end,

  after_disable = function ()
    -- the server answers (or finds the connection closed) after 1.5 s: no
    -- callback of the disabled extension may run, and nothing may crash
    test.sleep (2200)

    local report = test.extension_report (uuid)
    if report.failed > 0 then
      for _, check in ipairs (report.checks) do
        if not check.ok then
          fail ("%s: %s", check.name, check.message or "failed")
        end
      end
    end
  end,
}

server:stop ()
