# Example extensions

Each subdirectory is a complete extension in the packaging format described
in `docs/PLAN.md` (D13): a directory named after its uuid containing
`metadata.json` and `extension.lua`, plus optional `prefs.lua`,
`stylesheet.css`, `schemas/` and `locale/`. `hello/` is the canonical minimal
extension: it logs, runs a timer and shows how to connect to a hook only when
the running shell declares it. It has no `disable()` on purpose, to show that
everything created through `shell.*` is torn down automatically.

To install one for your user, copy the directory (the directory name must equal
the uuid in `metadata.json`) into the per-user extension directory and enable
it in the `org.gnome.shell` settings:

    mkdir -p ~/.local/share/gnome-shell/extensions
    cp -r hello ~/.local/share/gnome-shell/extensions/hello@gnome-lua.example
    gsettings set org.gnome.shell enabled-extensions "['hello@gnome-lua.example']"

(Adjust the `gsettings` call if you already have other enabled extensions.)

To try an extension straight from a source checkout, without installing it,
point `SHELL_LUA_EXTENSION_PATH` at the directory that contains it. The
variable is a colon-separated list of directories, searched in order, and
replaces the default search path:

    SHELL_LUA_EXTENSION_PATH=$PWD/subprojects/shell-lua/examples gnome-shell --nested

Directories listed there are treated like per-user extensions, and the
directory name must still equal the uuid, which is why `examples/` contains a
symlink `hello@gnome-lua.example` pointing at `hello/`. Log output appears in
the journal with the domain `shell-lua/hello@gnome-lua.example`.
