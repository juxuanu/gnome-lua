-- hello@gnome-lua.example: the preferences window of the extension.
--
-- prefs.lua only returns data, it is not run in the shell and has no access
-- to its objects: the window is made from this table by the extensions
-- service (docs/api/prefs.md). Every row is bound to one key of the schema
-- named by "settings-schema" in metadata.json (schemas/ holds it, compiled);
-- the extension sees the changes through shell.settings.extension().
--
-- Only `shell.api_version`, `shell.extension`, `shell.log`, `shell.gettext`
-- and the globals `_`, `ngettext` and `pgettext` exist here, so that the
-- strings are translated with the locale/ of the extension.
return {
  pages = {
    {
      title = _("General"),
      icon_name = "preferences-system-symbolic",
      groups = {
        {
          title = _("Greeting"),
          rows = {
            { type = "entry", key = "greeting", title = _("Message"),
              subtitle = _("Logged when the extension is enabled"), max_length = 80 },
          },
        },
        {
          title = _("Counter"),
          description = _("The counter is logged by a timer owned by the extension."),
          rows = {
            { type = "switch", key = "log-ticks", title = _("Log the counter") },
            { type = "spin", key = "interval", title = _("Interval"),
              subtitle = _("Seconds between two ticks"), min = 1, max = 3600, step = 1,
              depends_on = "log-ticks" },
            { type = "combo", key = "level", title = _("Log level"),
              choices = {
                { value = "info", label = _("Information") },
                { value = "debug", label = _("Debugging") },
              },
              depends_on = "log-ticks" },
          },
        },
      },
    },
  },
}
