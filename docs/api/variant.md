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
