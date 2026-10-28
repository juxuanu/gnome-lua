# gnome-lua

GNOME Shell with the JavaScript engine removed. Lua 5.5 is embedded directly in
the shell and extensions target a small, stable, versioned `shell.*` API instead
of the shell's internals. The shell's own UI logic is ported from JS into the
existing C core; no GObject Introspection binding is involved on the Lua side.

* [docs/PLAN.md](docs/PLAN.md): strategy, decisions, milestones, risks.
* [docs/PORT_STATUS.md](docs/PORT_STATUS.md): per-module JS → C tracking.

Status: planning. No code yet.
