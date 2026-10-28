# `shell.dbus`: D-Bus access (version 1, draft; capability `dbus`)

Scope: calling methods, reading and writing properties and receiving signals
of other D-Bus services, owning a bus name and exporting one object, all
without introspection data and without ever blocking the shell. Replaces
`Gio.DBusProxy.makeProxyWrapper`/`DBusExportedObject` use in extensions.
Every function in this document requires `"dbus"` in `metadata.json`
`capabilities`; without it the call raises
`shell.dbus.<fn>() requires the "dbus" capability`. Values are mapped by
`variant.md`; this document adds only what D-Bus needs on top.

Not in v1:
- Synchronous calls of any kind (they would stall the compositor; the
  watchdog would kill the extension first): everything is callback-based.
- Introspection (`org.freedesktop.DBus.Introspectable` of remote objects),
  generated or typed proxies, `GDBusProxy` property caches and
  `g-properties-changed`: a proxy here is only a `(name, path, interface)`
  triple.
- Unix file descriptors (`h`), `GDBusObjectManager`, peer-to-peer or private
  connections, other bus addresses, `org.freedesktop.DBus` bus-daemon calls
  (use `watch_name`), messages with `maybe` types (not valid D-Bus).
- Exported objects: more than one interface per object, writable properties,
  asynchronous method replies, caller identity (sender, credentials), fd
  arguments, sub-trees.
- Reserved names. `proxy` and `export` raise for destination or interface
  names `org.gnome.Shell` and `org.gnome.Shell.*` (the shell's external
  contract is reached through `shell.*`, and `org.gnome.Shell.Extensions` would
  allow installing code); `own_name` and `export` also raise for names and
  interfaces under `org.gnome.` and `org.freedesktop.` (impersonating system
  services). Names are validated with `g_dbus_is_name` /
  `g_dbus_is_interface_name`, paths with `g_variant_is_object_path`.
- No privilege boundary: `dbus` reaches whatever the user's session or the
  system bus policy allows (e.g. the systemd user manager can run commands).
  The extensions app displays it as such.

## Entry points

| Function | Returns |
|---|---|
| `shell.dbus.session()` | `shell.Bus` for the session bus |
| `shell.dbus.system()` | `shell.Bus` for the system bus (bus policy still applies) |
| `shell.dbus.variant(signature, value)` | a typed value for `v` positions, see Mapping |

A `shell.Bus` is a handle that performs no I/O to create. Every object created
from it is owned by the extension and torn down on disable (README):
subscriptions cancelled, names released, objects unexported, pending calls
cancelled (their callbacks never run afterwards).

## `shell.Bus`

| Method | Fields / args | Returns |
|---|---|---|
| `bus:proxy{...}` | `name` (well-known or unique name), `path`, `interface` (all required) | `shell.DBusProxy` |
| `bus:watch_name{...}` | `name`, `on_appeared(owner)`, `on_vanished()` (at least one) | `shell.Connection`; `:disconnect()` stops. `on_appeared` also fires at once if the name already has an owner, `on_vanished` if it has none |
| `bus:own_name{...}` | `name`, `allow_replacement` (false), `replace` (false), `on_acquired()`, `on_lost()` | `shell.BusName` |
| `bus:export{...}` | `path`, `interface_xml`, `methods`, `properties` (optional) | `shell.DBusObject` |

`shell.BusName`: `name` (ro), `acquired` (ro boolean), `release()`, `destroy()`
(same), `alive`.

## `shell.DBusProxy`

Properties (ro): `name`, `path`, `interface`, `bus` (`"session"|"system"`).

**`proxy:call{...}`** -> `shell.DBusCall` (`cancel()`, `pending` ro). Always
asynchronous.

| Field | Type | Notes |
|---|---|---|
| `method` | string, required | |
| `signature` | string | the body signature as D-Bus writes it, **without** outer parentheses (`"su"`, `"a{sv}"`, `"(ii)s"`). Required when `args` is non-empty |
| `args` | sequence | one element per complete type of `signature`, converted as in Mapping; `#args` must match, else the call raises before sending |
| `on_reply` | function(...) | the reply body spread into arguments (`()` -> none; `(su)` -> two); each mapped by variant.md |
| `on_error` | function(err) | `err = {name = "org.freedesktop.DBus.Error.ServiceUnknown", message = "...", remote = boolean}`; local failures use the standard names (`...Error.NoReply` on timeout, `...Error.Disconnected`). Without `on_error`, a failure is logged at warning level and does **not** count against the extension's error policy |
| `timeout_ms` | integer | default 5000, range 1..60000 |

Callbacks run on the main loop, under the watchdog, with the usual error
policy. Omitting both callbacks makes the call fire-and-forget (no reply is
awaited, `NO_REPLY_EXPECTED`).

| Method | Notes |
|---|---|
| `proxy:get_property{ name, on_reply(value), on_error(err) }` | `org.freedesktop.DBus.Properties.Get`; `v` unwrapped |
| `proxy:get_all{ on_reply(props), on_error(err) }` | `props` is a table keyed by property name |
| `proxy:set_property{ name, signature, value, on_done(), on_error(err) }` | `signature` is one complete type (no default: the wire type must be exact) |
| `proxy:on_signal(name_or_nil, fn)` | `fn(signal_name, ...)` with the body spread; `nil` = every signal of the interface from this name and path; returns `shell.Connection`. The subscription follows the name owner across restarts (`g_dbus_connection_signal_subscribe`) |
| `proxy:on_properties_changed(fn)` | `fn(changed, invalidated)`: `changed` a table keyed by property name, `invalidated` a sequence of names, for this interface; returns `shell.Connection` |

## `bus:export{...}`

| Field | Type | Notes |
|---|---|---|
| `path` | string | valid object path |
| `interface_xml` | string | `<node>` with exactly one `<interface>`; no child `<node>`; no `access="readwrite"` or `"write"` property; `Properties`/`Introspectable`/`Peer` are provided automatically |
| `methods` | table | `Name = function(...)` for XML methods; every function name must exist in the XML and a method with no function replies with `org.freedesktop.DBus.Error.UnknownMethod`. Arguments are decoded from the XML in-signature |
| `properties` | table | initial values for the XML properties, converted by their XML types; absent ones take the type's default (0, "", empty) |

A method function **returns its out-arguments in order** (one return value per
XML `out` arg, converted by the XML type). To fail, return
`nil, "org.example.Error.NotReady", "message"` (the name must be a valid error
name). A Lua error or a wrong return count produces
`org.freedesktop.DBus.Error.Failed` with a generic message (the traceback goes
to the log, never to the peer) and counts as an extension error. Handlers run
synchronously on the main loop under the watchdog.

`shell.DBusObject`: `path`, `interface` (ro); `obj:set_property(name, value)`
(converted by the XML type, emits `PropertiesChanged`),
`obj:emit_signal{ name, args }` (`args` a sequence matched to the XML signal
arguments), `obj:destroy()`, `alive`.

## Mapping

Results and signal arguments are converted with variant.md "GVariant to Lua".
Arguments, property values and returns are converted with variant.md "with a
target type": the target is the complete type taken from `signature` or the
XML, never guessed. In addition, for D-Bus:

- Integer types accept a Lua integer, or a float with an exact integer value,
  and are range-checked; `d` accepts any number; `b` only booleans; `s`, `o`,
  `g` strings, additionally checked as UTF-8, object path or signature.
- `ay` takes and returns a Lua string of raw bytes; other arrays and structs
  take sequences; `a{s*}` and `a{o*}` take and return string-keyed tables.
  Dictionaries with any other key type (`a{uv}`, `a{ii}`) are a sequence of
  two-element sequences `{key, value}` in both directions, since a Lua table
  cannot key them without ambiguity.
- `nil` is never valid in an argument (no `maybe` on the bus); `m*` and `h`
  in a signature raise.
- A `v` position (a bare `v`, `a{sv}` values, `av` elements, `set_property`
  with `signature = "v"`) takes either a Lua value, typed by the default rules
  (boolean `b`, integer `x`, number `d`, string `s`, sequence `av`, other table
  `a{sv}`), or `shell.dbus.variant(sig, value)` with `sig` a single complete
  type, e.g. `shell.dbus.variant("u", 5)`. Use the second form whenever the
  service expects a narrower integer type; a typed value outside a `v` position
  raises. Replies unwrap `v` and lose its inner type (variant.md).
- Needs an amendment of variant.md (`shell.dbus.variant` box, accepted by
  `shell_lua_to_variant` when the target is `v`).

## Example

```lua
local shell = require("shell")   -- metadata: "capabilities": ["dbus"]

local label = shell.ui.label{ text = "…", y_align = "center" }
shell.panel.add_indicator{ child = label, accessible_name = "Battery" }

local upower = shell.dbus.system():proxy{
  name = "org.freedesktop.UPower",
  path = "/org/freedesktop/UPower/devices/DisplayDevice",
  interface = "org.freedesktop.UPower.Device",
}
upower:get_property{ name = "Percentage", on_reply = function(p) label.text = ("%d%%"):format(p) end }
upower:on_properties_changed(function(changed)
  if changed.Percentage then label.text = ("%d%%"):format(changed.Percentage) end
end)

local session = shell.dbus.session()
local obj = session:export{
  path = "/org/example/Battery", interface_xml = [[
    <node><interface name="org.example.Battery">
      <method name="Get"><arg type="s" direction="out"/></method>
    </interface></node>]],
  methods = { Get = function() return label.text end },
}
local owner = session:own_name{ name = "org.example.Battery",
  on_lost = function() shell.log.warning("name taken by another process") end }

session:proxy{ name = "org.freedesktop.Notifications", path = "/org/freedesktop/Notifications",
               interface = "org.freedesktop.Notifications" }:call{
  method = "Notify", signature = "susssasa{sv}i",
  args = { "battery", 0, "battery-low-symbolic", "Battery", "Check the charger", {},
           { urgency = shell.dbus.variant("y", 1) }, 5000 },
  on_reply = function(id) shell.log.debug("notification %d", id) end,
  on_error = function(err) shell.log.warning("%s", err.name) end,
}
```
