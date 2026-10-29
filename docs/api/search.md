# `shell.search`: search providers in the overview (version 1.1)

Scope: adding a section of results to the search of the overview: the user
types, the shell asks the provider for results, shows them in a list headed by
the name of the provider, and tells the provider which one was activated.
Models the search provider of the JS shell (`SearchProvider` of
`js/ui/search.js`: `getInitialResultSet ()`, `getSubsearchResultSet ()`,
`getResultMetas ()`, `activateResult ()`, `launchSearch ()`, registered with
`Main.overview.searchController.addProvider ()`), reduced to one asynchronous
call and plain tables. The C side is a `ShellSearchProvider`
(`src/ui/search.h`) implemented by `src/lua-api/shell-lua-api-search.c`, which
the `ShellSearchResultsView` of the overview consumes exactly like the
providers of installed applications (`src/ui/remote-search.c`). Observing what
the user types, without providing results, is the hook `search.query`
(hooks.md); both need the same capability.

Since: **1.1**. Every row below is new in 1.1; an extension that uses it
declares `"shell-api": "1.1"` and `"capabilities": ["search"]`.

Not in v1.1:
- Result widgets of your own, grid results, per-result context menus, drag and
  drop of results, a progress indicator, and results that change after they
  were delivered (`respond` answers once, below).
- A "narrowing" search: the provider is asked for the full terms every time
  (the `getSubsearchResultSet ()` of the JS protocol is not exposed), and it is
  never given the results of the previous search.
- Results of the provider in the application grid, in the dash, or anywhere
  but the search of the overview; a switch for the provider in the search
  settings of the Settings application (an extension's provider is on while
  the extension is enabled).
- Telling the provider that a search was cancelled (the `respond` of a
  cancelled search is silently a no-op, below) or that the overview closed.

## Status

**1.1, implemented.** `src/lua-api/shell-lua-api-search.c` (the module, the
handle type and the `ShellSearchProvider` implementation) over
`ShellSearchResultsView` (`src/ui/search.c`) through
`shell_search_controller_add_provider ()` (`src/ui/search-controller.c`).
Conformance test: `tests/api/search.lua` with the fixture
`tests/api/fixtures/search@test`.

## Capability

`shell.search.add_provider` needs the capability `search` (README.md,
"Capabilities"): a provider receives what the user types, the same data as the
hook `search.query`. Without it the call raises
`shell.search.add_provider requires the "search" capability`, before anything
is created.

## `shell.search.add_provider{...}` -> `shell.SearchProvider` | `nil, message`

| Field | Type | Default | Notes |
|---|---|---|---|
| `id` | string | required | identifies the provider among the extension's own; not shown. A second live provider of the same extension with the same `id` raises. The shell sees it as `<uuid>:<id>` |
| `name` | string | required | the title in the header of the section, and its accessible name. Not empty |
| `icon_name` / `icon_file` | string | `icon_name = "application-x-addon-symbolic"` | the icon in the header; at most one. `icon_file` is a path relative to the extension directory, `.svg` or `.png`, as in ui.md |
| `on_search` | function(provider, terms, respond) | required | the user typed: look for `terms` and answer with `respond`, below |
| `on_activate` | function(provider, result_id, terms) | required | the user activated the result `result_id` (a click, or Enter on the selected one), found by a search for `terms` |
| `on_launch` | function(provider, terms) | nil | the user clicked the header of the section ("show all results"). When it is nil the header is not a button and the section never shows the "N more" count; when it is given, "N more" is shown whenever the provider returned more results than the five that fit |

A provider is a handle to something on screen: its callbacks get the
`shell.SearchProvider` first (README.md rule 6). `terms` is a sequence of
strings, the words the user typed, split as the shell splits them for its own
search (the `terms` of the hook `search.query`); it is never empty. At most 8
providers per extension (raises beyond, like the limits of the other UI
objects).

The call returns `nil, message` when the search of the overview does not exist
(a session mode without an overview, README.md: extensions run in the `user`
mode by default, so this does not happen in practice); misuse raises.

The section of the provider is placed after the applications and after the
search providers of installed applications, and the sections of extensions are
ordered by the position of the extension in `enabled-extensions`, then by the
order of creation (README.md, "Placement and ordering", without the
`index`). The result list shows at most 5 results.

### `on_search(provider, terms, respond)`

The shell calls it once per search, after the shell's own debounce of 150 ms
(like `search.query`), also when the new terms only extend the old ones; the
sections of all providers are searched in parallel. `respond` is the only way
to answer:

**`respond(results)`**: `results` is a sequence of tables (empty for no
results) in the order they are shown. The first `respond` of a search answers
it; later calls for the same search are ignored. It can be called before
`on_search` returns or later, from a timer, a D-Bus reply or any other
callback; nothing waits for it. Until it is called the section shows no
results, and the page shows "Searching" while no other section has results. A
search that is not answered within 10 s counts as answered with no results.
`respond` becomes a no-op, returning nothing and raising nothing, when:

- a newer search started (the terms changed) or the search was cancelled (the
  entry was cleared, the overview was hidden): **a new search cancels the
  previous**, and the answer to the old one is dropped;
- the provider was destroyed, or the extension was disabled.

| Field of a result | Type | Default | Notes |
|---|---|---|---|
| `id` | string | required | passed back to `on_activate`. Not empty; a repeated `id` in the same answer is dropped (the first one stays) |
| `name` | string | required | the title of the row. Not empty |
| `description` | string | nil | a second line (only the first line of the text is shown; the words of the search are highlighted in it) |
| `icon_name` / `icon_file` | string | none | the icon of the row; at most one. `icon_file` is an absolute path of an existing `.svg` or `.png` file, or a path relative to the extension directory |
| `clipboard_text` | string | nil | copied to the clipboard when the result is activated (after `on_activate` returned), as the results of the calculator do |

At most **32** results are kept; the rest of the sequence is dropped. The
results are described again for every search: the name, description and icon
of a result with the same `id` as one of the previous search are the new ones.
Misuse raises (in `respond`, so it is an error of the callback that called it,
README.md rule 8): `results` that is not a sequence of tables, a result without
`id` or `name` or with a field of the wrong type, an unknown field (the error
names it). A raised error in `respond` answers the search with no results.

An error in `on_search`, or a call while the extension is being disabled,
answers the search with no results.

### `on_activate` and `on_launch`

They run from the activation of the user, in the main loop; the overview hides
after they return (like for every search result). `on_launch` is only called
for a provider that has one.

## `shell.SearchProvider`

| Property | Type | Access |
|---|---|---|
| `id` | string | ro (the `id` of the constructor) |
| `alive` | boolean | ro |

Methods: `destroy()`. The section is removed from the search at once; a
search in progress is cancelled and its answer dropped. The handle is dead from
then on. At disable the context destroys the providers like every owned object
(README.md, "Ownership and teardown"): the sections disappear, `on_search` is
never called again, and nothing of the extension stays in the results.

## Example

```lua
-- metadata.json: "shell-api": "1.1", "capabilities": ["search"]
local shell = require("shell")

local CITIES = { paris = 3600, tokyo = 9 * 3600, lima = -5 * 3600 }  -- offsets from UTC in seconds (no DST here)

shell.search.add_provider{
  id = "world-clock",
  name = "World clock",
  icon_name = "preferences-system-time-symbolic",
  on_search = function(provider, terms, respond)
    local results = {}
    for _, word in ipairs(terms) do
      for city, offset in pairs(CITIES) do
        if city:find(word:lower(), 1, true) == 1 then
          -- shell.time knows the local zone and UTC only: other zones are an offset you add
          local now = shell.time.format("%H:%M", { at = shell.time.now() + offset, utc = true })
          results[#results + 1] = {
            id = city, name = city:sub(1, 1):upper() .. city:sub(2),
            description = "UTC" .. string.format("%+d", offset // 3600) .. ", " .. now, clipboard_text = now,
            icon_name = "preferences-system-time-symbolic",
          }
        end
      end
    end
    respond(results)
  end,
  on_activate = function(provider, id, terms)
    shell.log("showed the time of %s", id)
  end,
}
```
