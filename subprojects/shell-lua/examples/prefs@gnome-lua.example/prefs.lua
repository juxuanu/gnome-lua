-- prefs@gnome-lua.example: a tour of prefs.lua (docs/api/prefs.md).
--
-- The file only returns data. Every row is bound to one key of the schema in
-- schemas/ (metadata.json: "settings-schema"); the preferences window is
-- made from this table by the extensions service, the extension runs no code
-- there. Open it with `gnome-extensions prefs prefs@gnome-lua.example`.
--
-- Only `shell.api_version`, `shell.extension`, `shell.log`, `shell.gettext`
-- and the globals `_`, `ngettext` and `pgettext` exist here.
return {
  title = _("Preferences example"),
  pages = {
    {
      title = _("General"),
      icon_name = "preferences-system-symbolic",
      groups = {
        {
          title = _("Banner"),
          description = _("What the extension says when it starts."),
          rows = {
            -- a boolean key: a switch
            { type = "switch", key = "show-banner", title = _("Show the banner") },
            -- a string key: an entry; active while the key it depends on is true
            { type = "entry", key = "greeting", title = _("Greeting"),
              subtitle = _("The first words of the banner"), max_length = 40,
              depends_on = "show-banner" },
          },
        },
        {
          title = _("Ticks"),
          rows = {
            -- a string key with <choices>: a combo; every value must be a choice
            { type = "combo", key = "format", title = _("Detail"),
              choices = {
                { value = "short", label = _("Short") },
                { value = "long", label = _("Long") },
              } },
            -- an integer key: a combo works on integers too
            { type = "combo", key = "columns", title = _("Columns"),
              choices = {
                { value = 1, label = _("One") },
                { value = 2, label = _("Two") },
                { value = 4, label = _("Four") },
              },
              -- active while another key has this value
              depends_on = { key = "format", value = "long" } },
            -- an integer key: a spin row; the bounds default to the <range>
            { type = "spin", key = "interval", title = _("Interval"),
              subtitle = _("Seconds between two ticks"), min = 1, max = 60, step = 1 },
          },
        },
      },
    },
    {
      title = _("Appearance"),
      icon_name = "applications-graphics-symbolic",
      groups = {
        {
          rows = {
            -- a double key: a spin row with digits
            { type = "spin", key = "opacity", title = _("Opacity"), step = 0.05, digits = 2 },
            -- a string key holding a CSS colour
            { type = "color", key = "accent", title = _("Accent colour") },
            { type = "color", key = "highlight", title = _("Highlight"), alpha = true },
            -- a string array key holding accelerators; inactive while the banner is off
            { type = "keybinding", key = "toggle-shortcut", title = _("Shortcut"),
              max = 2, allow_empty = true, depends_on = "!show-banner" },
          },
        },
      },
    },
  },
}
