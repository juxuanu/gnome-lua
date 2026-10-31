-- emoji-search@gnome-lua.example: a search provider (docs/api/search.md,
-- capability "search").
--
-- A provider gets what the user types in the overview, which is why it needs
-- a capability. This one looks the words up in a small table of emoji and
-- answers with the matches; activating a result copies the emoji to the
-- clipboard (clipboard_text), as the results of the calculator do.
local shell = require("shell")

-- emoji, then the words it is found by
local EMOJI = {
  { "😀", "grin smile happy face" },
  { "😂", "joy laugh tears face" },
  { "😉", "wink face" },
  { "😍", "love heart eyes face" },
  { "🤔", "think thinking face" },
  { "😢", "cry sad tear face" },
  { "😡", "angry rage face" },
  { "😴", "sleep tired face" },
  { "👍", "thumbs up yes ok good" },
  { "👎", "thumbs down no bad" },
  { "👋", "wave hello bye hand" },
  { "🙏", "pray thanks please hands" },
  { "👏", "clap applause hands" },
  { "❤️", "heart love red" },
  { "💔", "broken heart" },
  { "🔥", "fire hot lit" },
  { "⭐", "star" },
  { "✨", "sparkles shine" },
  { "🎉", "party tada celebrate" },
  { "🎂", "cake birthday" },
  { "☕", "coffee tea hot drink" },
  { "🍺", "beer drink" },
  { "🍕", "pizza food" },
  { "🐱", "cat kitten animal" },
  { "🐶", "dog puppy animal" },
  { "🌍", "earth world globe" },
  { "☀️", "sun sunny weather" },
  { "🌧️", "rain weather cloud" },
  { "❄️", "snow cold weather" },
  { "✅", "check done yes" },
  { "❌", "cross no wrong" },
  { "⚠️", "warning caution" },
  { "💡", "idea light bulb" },
  { "🚀", "rocket launch fast" },
  { "🐛", "bug insect" },
  { "🔒", "lock locked secure" },
}

-- Does every term start one of the words of @words?
local function matches(words, terms)
  for _, term in ipairs(terms) do
    if not (" " .. words):find(" " .. term:lower(), 1, true) then
      return false
    end
  end
  return true
end

local ext = {}

function ext.enable()
  shell.search.add_provider{
    id = "emoji",
    name = _("Emoji"),
    icon_name = "face-smile-symbolic",

    -- Answered at once: the table is small. A provider that looks things up
    -- on D-Bus or the network answers from the reply instead
    on_search = function(_, terms, respond)
      local results = {}

      for i, entry in ipairs(EMOJI) do
        local emoji, words = entry[1], entry[2]

        if matches(words, terms) then
          results[#results + 1] = {
            id = tostring(i),
            name = emoji .. "  " .. words:match("^%S+"),
            description = words,
            clipboard_text = emoji,
          }
        end
      end
      respond(results)
    end,

    -- The shell copies clipboard_text after this returns
    on_activate = function(_, id)
      shell.log.debug("copied %s", EMOJI[tonumber(id)][1])
    end,
  }
end

return ext
