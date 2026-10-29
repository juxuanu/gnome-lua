-- Conformance test of docs/api/search.md (and the capability `search` of
-- README.md): see tests/api/README.md. The assertions about arguments,
-- handles and what the providers were told are in the fixtures
-- tests/api/fixtures/search@test and search-nocap@test, run by the extension
-- manager of the shell; this script types the searches into the search entry
-- of the overview and compares what the results view shows (the number of
-- results of the section of a provider, the meta of a result) with what the
-- providers answered, activates results, and asks the fixture for its
-- verdicts with steps.

local harness = dofile(((os.getenv ("SHELL_TEST_SCRIPT") or ""):match ("^(.*)/[^/]*$") or ".") .. "/harness.lua")

local TIMEOUT = 10000

local uuid = "search@test"
local CALC = uuid .. ":calc"       -- the id of the provider the search view knows
local SECOND = uuid .. ":second"

local function fail (fmt, ...)
  error (string.format (fmt, ...), 0)
end

local function settle ()
  test.wait_leisure ()
  test.sleep (300)
  test.wait_leisure ()
end

test.wait_startup ()

-- Without the capability, add_provider raises
harness.run { uuid = "search-nocap@test" }

local function overview_shown ()
  return main.overview.visible and not main.overview.animation_in_progress
end

local function show_overview ()
  local overview = main.overview

  if overview_shown () then
    return
  end
  if overview.visible then
    -- on its way out or in
    settle ()
    if overview_shown () then
      return
    end
  end
  local shown = test.watch (overview, "shown")
  overview:show ()
  test.wait (shown, TIMEOUT)
  settle ()
end

local function hide_overview ()
  local overview = main.overview

  if overview.visible then
    local hidden = test.watch (overview, "hidden")
    overview:hide ()
    test.wait (hidden, TIMEOUT)
  end
  settle ()
end

local function reported (name)
  for _, check in ipairs (test.extension_report (uuid).checks) do
    if check.name == name then
      return true
    end
  end
  return false
end

harness.run {
  uuid = uuid,
  timeout = 30000,

  -- the fixture causes one error on purpose (the provider that raises)
  expect_errors = 1,

  -- the overview and its search have actors of their own
  actors = false,

  drive = function (c)
    local controller

    c.wait_done ("main")

    -- the session starts in the overview; the search wants it shown
    hide_overview ()
    show_overview ()
    controller = main.overview.search_controller

    local function wait_reported (name)
      c.wait_until (function () return reported (name) end, "the extension to report '" .. name .. "'")
    end

    -- types a search; waits until a provider was asked
    local function search (text, asked)
      controller:search (text)
      wait_reported ("search: " .. (asked or text))
    end

    -- waits until the section of the provider shows n results and no provider is still working
    local function wait_results (id, n)
      c.wait_until (function ()
        return not controller:search_in_progress () and controller:n_results (id) == n
      end, function ()
        return string.format ("%d results of %s (it shows %d, searching: %s)", n, id, controller:n_results (id),
                              tostring (controller:search_in_progress ()))
      end)
    end

    local function meta (id, index)
      local result = controller:get_result (id, index)

      if result == nil then
        fail ("%s shows no result number %d", id, index)
      end
      return result.meta_info
    end

    local function expect (got, want, what)
      if got ~= want then
        fail ("%s: expected %s, got %s", what, tostring (want), tostring (got))
      end
    end

    -- ------------------------------------------------------------------
    -- A search that is answered at once: 7 results, the list shows 5
    -- ------------------------------------------------------------------
    search ("echo foo")
    wait_results (CALC, 5)
    wait_results (SECOND, 1)

    local first = meta (CALC, 1)
    expect (first.id, "r1", "id of the first result")
    expect (first.name, "Result 1 for echo foo", "name of the first result")
    expect (first.description, "description 1\nsecond line", "description of the first result")
    expect (first.clipboard_text, "clip", "clipboard_text of the first result")
    expect (meta (CALC, 5).name, "Result 5 for echo foo", "name of the fifth result")
    expect (controller:get_result (CALC, 6), nil, "a sixth result")
    expect (meta (SECOND, 1).name, "The only result of echo foo", "name of the result of the second provider")
    c.step ("check-search")

    -- the header of the second provider launches the search
    controller:get_result (SECOND, 1):launch_search ()
    c.step ("check-launch")

    -- activating a result calls on_activate, then the overview hides
    controller:get_result (CALC, 1):activate ()
    c.step ("check-activate")
    c.wait_until (function () return not main.overview.visible end, "the overview to hide after the activation")
    settle ()
    show_overview ()

    -- ------------------------------------------------------------------
    -- An answer that comes later, from outside of on_search
    -- ------------------------------------------------------------------
    search ("later bar")
    if controller:n_results (CALC) ~= 0 or not controller:search_in_progress () then
      fail ("the section shows results, or the search ended, before the provider answered")
    end
    c.step ("respond-later")
    wait_results (CALC, 2)
    expect (meta (CALC, 1).name, "Result 1 for later bar", "name of the first result of the late answer")

    -- ------------------------------------------------------------------
    -- A new search cancels the previous: its respond does nothing
    -- ------------------------------------------------------------------
    search ("stale one")
    search ("stale two")
    c.step ("respond-stale")
    wait_results (CALC, 1)
    settle ()
    expect (controller:n_results (CALC), 1, "results of the section after the answer of the stale search")
    expect (meta (CALC, 1).name, "Stale two", "name of the result")

    -- ------------------------------------------------------------------
    -- The three kinds of icon, a repeated id, no results, a wrong answer
    -- ------------------------------------------------------------------
    search ("icons")
    wait_results (CALC, 4)
    expect (meta (CALC, 3).name, "By absolute file", "name of the third result")
    expect (meta (CALC, 4).name, "No icon", "name of the fourth result (the repeated id is dropped)")
    test.wait_leisure ()

    search ("none")
    wait_results (CALC, 0)

    search ("bad")
    wait_results (CALC, 0)
    c.step ("check-bad")

    for n = 1, 10 do
      search ("invalid " .. n)
    end
    wait_results (CALC, 0)
    c.step ("check-invalid")

    -- a provider that raises: the search is answered with no results
    local log = c.capture_log (function ()
      search ("error one")
      wait_results (CALC, 0)
    end)
    local logged = false
    for _, entry in ipairs (log) do
      if entry.message:find ("the extension fails on purpose", 1, true) then
        logged = true
      end
    end
    if not logged then
      fail ("the error of on_search was not logged")
    end

    -- ------------------------------------------------------------------
    -- Destroying a provider removes its section; its id is free again
    -- ------------------------------------------------------------------
    c.step ("destroy-second")
    search ("echo foo")
    wait_results (CALC, 5)
    expect (controller:n_results (SECOND), 0, "results of the destroyed provider")
    c.step ("check-second-silent")

    c.step ("re-add-second")
    search ("echo bar")
    wait_results (CALC, 5)
    wait_results (SECOND, 1)
    expect (meta (SECOND, 1).name, "Again", "name of the result of the provider added again")

    -- end the search, leave the overview
    controller:search ("")
    settle ()
    hide_overview ()
  end,

  -- the providers are gone: they are not asked, and nothing is shown for them
  after_disable = function (c)
    local controller = main.overview.search_controller
    local n = #test.extension_report (uuid).checks

    show_overview ()
    controller:search ("echo foo")
    c.wait_until (function () return not controller:search_in_progress () end, "the search to end")
    test.sleep (300)

    if controller:n_results (CALC) ~= 0 or controller:n_results (SECOND) ~= 0 then
      fail ("the sections of the disabled extension still show results")
    end
    if #test.extension_report (uuid).checks ~= n then
      fail ("a provider of the disabled extension was asked")
    end

    controller:search ("")
    settle ()
    hide_overview ()
  end,
}
