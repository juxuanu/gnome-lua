# GVariant ↔ Lua mapping (version 1)

| GVariant | Lua |
|---|---|
| `b` | boolean |
| `y n q i u x t h` | integer |
| `d` | number |
| `s o g` | string |
| `ay` | string (raw bytes) |
| `v` | unwrapped inner value |
| `m*` | nil or inner value |
| `a{s*}`, `a{o*}` | table keyed by string |
| other `a*`, tuples | sequence (1-based) |

Lua → GVariant without a target type: boolean→`b`, integer→`x`, number→`d`,
string→`s`, sequence→`av`, other table→`a{sv}`, nil→error (ambiguous).
With a target type the value is converted or an error names the mismatch.

## Typed values

`shell.dbus.variant(signature, value)` (dbus.md) returns an opaque Lua value
that wraps a GVariant of the exact type `signature` (a single complete type;
`value` is converted "with a target type" when it is made, so a wrong value
raises there). It is accepted where the target type is `v`, alone or as an
element of an array or a dictionary that is converted to `av` or `a{sv}`, and
becomes that `v`'s content with its type unchanged. Everywhere else, and in the
conversion without a target type, it raises. It is immutable, compares by
identity and prints as `<variant 'u'>`. Nothing ever produces one: values
that come back from GVariant are plain Lua values and lose the inner type of
a `v` (above).

Status: implemented (M7): `shell_lua_push_variant_box ()` and
`shell_lua_variant_box_peek ()` in `shell-lua-variant.h`, tests in
`subprojects/shell-lua/tests/test-variant.c`; the Lua function is
`shell-lua-api-dbus.c`.
