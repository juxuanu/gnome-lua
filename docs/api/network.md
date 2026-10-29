# `shell.network`: HTTP requests (version 1.1; capability `network`)

Scope: asynchronous HTTP and HTTPS requests with a status, headers and a body
that is read into memory, for extensions that show data from a web service
(weather, a build status, a feed) or that notify a webhook. Models
`Soup.Session` as GJS extensions used it (`send_and_read_async`), without the
parts that need a loop of their own. Every function in this document is
**Since 1.1**: a shell that reports `shell.api_version.minor == 0` has no
`shell.network` (`shell.network == nil`), and an extension that declares
`"shell-api": "1.1"` in its metadata can rely on it. It requires `"network"` in
`metadata.json` `capabilities`; without it the call raises
`shell.network.request() requires the "network" capability`, before anything
is sent.

Status: implemented by `src/lua-api/shell-lua-api-network.c` (libsoup 3, in
libshell); conformance test `tests/api/network.lua` with the fixture
`tests/api/fixtures/network@test`. The CI container has no internet, so the
test script starts a local server of its own (`test.http_server`, in
`src/shell-test-runner.c`) and the fixture talks to that.

Not in 1.1:
- Synchronous requests of any kind: everything is callback based and the shell
  never waits for the network.
- Streaming: no progress callback, no incremental body, no upload from a file,
  no response bigger than 64 MiB (the body is a Lua string, which also counts
  against the memory limit of the extension, SANDBOX.md).
- WebSockets, Server-Sent Events, HTTP/3, raw sockets, listening for
  connections (`network` is for clients), name resolution, mDNS.
- Cookies: none is stored, sent or shared between requests (a `Set-Cookie`
  header of a response is visible in `response.headers`, nothing else), and
  there is no cache. State that must survive belongs in `shell.settings` or,
  with the `fs` capability, in a file.
- HTTP authentication dialogs, client certificates, custom certificate
  authorities, switching certificate validation off, pinning, proxy settings
  of its own (the system proxy configuration of the user applies). A `401` is
  an ordinary response; send an `Authorization` header yourself.
- Helpers for JSON, forms or multipart bodies: build the string in Lua.
- A way to tell whether the machine is online (a request that cannot connect
  fails with `connection` or `resolve`).
- No privilege boundary: `network` reaches whatever the user's machine can
  reach, including services on `localhost` and in the local network, and a
  plain `http:` URL is sent in the clear. The extensions app shows the
  capability as such.

## `shell.network.request{...}`

Starts a request and returns a `shell.Request` at once. Nothing is sent
synchronously and no callback runs before `request` has returned: the
completion always arrives from the main loop. Exactly one of `on_done` and
`on_error` runs, once, unless the request is destroyed or the extension is
disabled first (then neither does).

| Field | Type | Default | Since | Notes |
|---|---|---|---|---|
| `url` | string | required | 1.1 | absolute `http:` or `https:` URL with a host, at most 8192 bytes, without user name or password (`https://user:pass@host/` raises: send an `Authorization` header). Any other scheme raises |
| `method` | `"GET"`, `"POST"`, `"PUT"`, `"DELETE"`, `"HEAD"`, `"PATCH"` | `"GET"` | 1.1 | exactly as written (upper case); anything else raises |
| `headers` | table `{ Name = "value" }` | none | 1.1 | at most 64 entries; values are strings. See "Request headers" |
| `body` | string | none | 1.1 | binary safe (may contain NUL). `GET` and `HEAD` with a body raise |
| `content_type` | string | `"application/octet-stream"` when there is a `body` | 1.1 | the `Content-Type` of the body. Raises without a `body`, or together with a `Content-Type` entry in `headers` (give one of them) |
| `timeout_ms` | integer | 30000 | 1.1 | range 1..3600000. Total time from the call to the end of the body, redirects included; not an idle timeout |
| `max_bytes` | integer | 8388608 (8 MiB) | 1.1 | range 0..67108864 (64 MiB). The largest response body accepted, after content decoding |
| `on_done` | function(response) | nil | 1.1 | the request got an HTTP response, whatever its status |
| `on_error` | function(err) | nil | 1.1 | the request did not get one. Without it a failure is logged as a message (domain `shell-lua/<uuid>`, not a warning) and does **not** count against the extension's error policy |

Both callbacks are optional (a fire-and-forget `POST`). Argument errors raise
before anything is created, naming the field (README convention 1); a request
that was started never raises afterwards.

### Request headers

Names are HTTP tokens (letters, digits and ``!#$%&'*+-.^_`|~``), values are
single-line strings without control characters other than tab. Two names that
differ only in case raise. The names the HTTP implementation owns raise
`header '<name>' cannot be set`: `Host`, `Content-Length`, `Transfer-Encoding`,
`Connection`, `Keep-Alive`, `Proxy-Connection`, `Proxy-Authorization`,
`Upgrade`, `TE`, `Trailer`, `Expect` and `Accept-Encoding` (the body is decoded
for you, below). `User-Agent` may be replaced; by default it is
`gnome-lua/<uuid>` (the uuid of the extension, nothing else is added), and
`Accept: */*` is sent unless you give one.

### Response

`on_done(response)` receives a table:

| Field | Type | Since | Notes |
|---|---|---|---|
| `status` | integer | 1.1 | the HTTP status code, 100..599 |
| `reason` | string | 1.1 | the reason phrase (the standard one for the status when the protocol has none, as HTTP/2; possibly empty) |
| `headers` | table `{ name = "value" }` | 1.1 | names in lower case; a header that was sent more than once is one entry with the values joined by `", "` in order (so several `set-cookie` headers cannot be told apart: no cookie API is built on this) |
| `body` | string | 1.1 | the body as bytes, not checked for UTF-8; empty for `HEAD`, `204` and `304` |
| `url` | string | 1.1 | the URL the response came from, after redirects |

**HTTP error statuses are responses, not errors**: a `404` or a `500` reaches
`on_done` with its body. `on_error` is for requests that produced no response.

Redirects (`3xx` with a `Location`) are followed, at most 10 of them, with the
usual change of method (`301`, `302` and `303` turn a `POST` into a `GET`, and
the body is not sent again); more than 10, a redirect to a scheme other than
`http:` or `https:`, or one without a usable `Location` fails with `other`.
`307` and `308` keep the method; a request with a `body` is not re-sent by
every version of the HTTP library, and then the `307` or `308` itself is the
response (look at `status`). Compressed responses (`Content-Encoding` gzip and
deflate, and brotli where the library has it) are decoded; `max_bytes` counts
the decoded bytes, and a response whose `Content-Length` already exceeds
`max_bytes` fails with `too-large` before the body is read.

### Errors

`on_error(err)` receives `{ message = string, code = string }`. `message` is
for logs and is not promised (README: versioning); `code` is one of:

| `code` | When |
|---|---|
| `"cancelled"` | `request:cancel()` was called |
| `"timeout"` | `timeout_ms` elapsed before the response was complete, or the system reported a timeout while connecting |
| `"too-large"` | the body is bigger than `max_bytes` (declared or actually received); nothing of it is delivered |
| `"tls"` | the certificate is not valid for the host or not trusted, or the handshake failed |
| `"resolve"` | the host name could not be resolved |
| `"connection"` | refused, unreachable, reset or closed by the peer, also in the middle of a body; failures of a proxy |
| `"other"` | anything else: too many redirects, an unusable redirect, a malformed response |

Callbacks run on the main loop under the watchdog and the usual error policy
(README convention 8). They take only the result (one-shot completion
callbacks pass no handle).

## `shell.Request`

| Member | Type | Since | Notes |
|---|---|---|---|
| `request:cancel()` | method | 1.1 | stops the request; `on_error` then runs once from the main loop with `code = "cancelled"` (never synchronously inside `cancel`). Does nothing when the request has finished, was cancelled already or is dead |
| `request:destroy()` | method | 1.1 | stops the request and forgets it: **no** callback runs. Idempotent |
| `request.alive` | boolean, ro | 1.1 | true while the request is in flight, and while its callback runs; false once the callback has returned, after `destroy()` and at disable |

A request is an owned object (README "Ownership and teardown"): it counts
against the cap of owned objects while it is in flight and stops counting when
it has completed. At disable the extension's requests are cancelled without any
callback and the connections of the extension are closed; nothing the
extension started continues after `disable()` returns. All requests of an
extension share one HTTP session (created at the first call: keep-alive
connections are reused, and the libsoup limits on parallel connections per
host apply, so a request may wait for its turn; the wait counts against
`timeout_ms`). The session belongs to the extension: nothing, cookies or
connections, is shared with other extensions or with the shell.

## Example

```lua
local shell = require("shell")   -- metadata: "shell-api": "1.1", "capabilities": ["network"]

local label = shell.ui.label{ text = "…", y_align = "center" }
shell.panel.add_indicator{ child = label, accessible_name = "Weather" }

local request
local function refresh()
  request = shell.network.request{
    url = "https://wttr.in/Berlin?format=%t",
    headers = { Accept = "text/plain" },
    timeout_ms = 10000,
    max_bytes = 4096,
    on_done = function(response)
      if response.status == 200 then
        label.text = response.body
      else
        label.text = ("HTTP %d"):format(response.status)
      end
    end,
    on_error = function(err)
      label.text = "offline"
      shell.log.debug("weather: %s (%s)", err.message, err.code)
    end,
  }
end

refresh()
shell.timer.every{ ms = 15 * 60 * 1000, callback = refresh }

-- a POST with a JSON body, the answer does not matter
shell.network.request{
  method = "POST", url = "https://example.org/hook",
  content_type = "application/json", body = '{"event":"enabled"}',
}
```
