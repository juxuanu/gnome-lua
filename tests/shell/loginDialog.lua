-- This script tests the login screen (src/gdm/login-dialog.c,
-- src/gdm/user-verifier.c): the shell runs as GDM's greeter, against
-- tests/greeter-mock.py, which stands in for GDM and AccountsService and
-- logs what it is asked to GREETER_MOCK_LOG. Enter on the user list picks
-- the first user: GDM must then be asked to verify that user (no user of the
-- mock has a session to reauthenticate, as at a real login screen), not a
-- user to be named. A wrong password shows an error, then the prompt asks
-- again; Escape while the error shows goes back to the user list instead,
-- cancelling the verification once: the
-- end of the error's wait used to reset the prompt inside the reset of the
-- Escape, which cancelled it again and began a verification that the outer
-- reset abandoned. Typing the password logs in.

local TIMEOUT = 10000
local PASSWORD = "gnome"

local mock_log_path = assert (os.getenv ("GREETER_MOCK_LOG"),
                              "GREETER_MOCK_LOG is not set: run under gnomeshell_dbusrunner.py")

local results = {
  verified_user = nil,
  asked_for_username = false,
  password_refused = false,
  escape_stopped = false,
  password_accepted = false,
  session_started = false,
}

test.finish (function ()
  assert (not results.asked_for_username,
          "Choosing a user asked for the username")
  assert (results.verified_user ~= nil, "Choosing a user did not begin its verification")
  assert (results.password_refused, "The wrong password was not refused")
  assert (results.escape_stopped, "Escape after the wrong password did not stop the verification")
  assert (results.password_accepted, "The password was not accepted")
  assert (results.session_started, "The session was not started")
end)

-- What the mock was asked so far
local function mock_log ()
  local file = io.open (mock_log_path)
  if file == nil then
    return ""
  end
  local text = file:read ("a")
  file:close ()
  return text
end

local function wait_mock (pattern, description)
  local match
  test.wait_for (function ()
    match = mock_log ():match (pattern)
    return match ~= nil
  end, description, TIMEOUT)
  return match
end

local function type_text (text)
  for c in text:gmatch (".") do
    test.press_accelerator (c)
    test.sleep (50)
  end
end

local function count (pattern)
  local n = 0
  for _ in mock_log ():gmatch (pattern) do
    n = n + 1
  end
  return n
end

local function count_verifications ()
  return count ("gdm%-password: verifying ")
end

test.wait_startup ()

test.log ("Running login dialog test")

-- The user list comes from AccountsService once the dialog is up
wait_mock ("RegisterDisplay called", "the login screen to register with GDM")
test.sleep (1000)
test.wait_leisure ()

test.log ("Choosing the first user")
test.press_accelerator ("Return")
local begun = wait_mock ("gdm%-password: (%a[^\n]*)\n", "the verification to begin")
results.asked_for_username = begun == "asking for the username"
results.verified_user = begun:match ("^verifying (%S+)$")
if results.verified_user == nil then
  return
end
test.wait_leisure ()

local user_pattern = results.verified_user:gsub ("%p", "%%%0")

local function refuse ()
  local refused = count ("gdm%-password: " .. user_pattern .. " refused")

  test.log ("Typing a wrong password")
  type_text ("wrong")
  test.press_accelerator ("Return")
  test.wait_for (function () return count ("gdm%-password: " .. user_pattern .. " refused") > refused end,
                 "the wrong password to be refused", TIMEOUT)
end

refuse ()
local verifications = count_verifications ()
test.wait_for (function () return count_verifications () > verifications end,
               "the prompt to ask again once the error was shown", TIMEOUT)
results.password_refused = true
test.wait_leisure ()

refuse ()

-- The error stays for the time it takes to read it; then the prompt would
-- ask again. Escape goes back to the user list instead.
test.sleep (300)
verifications = count_verifications ()
local cancels = count ("verification cancelled")
test.press_accelerator ("Escape")
test.wait_for (function () return count ("verification cancelled") > cancels end, "Escape to cancel", TIMEOUT)
test.sleep (4000)
test.wait_leisure ()
assert (count ("verification cancelled") == cancels + 1,
        "Escape cancelled the verification more than once:\n" .. mock_log ())
assert (count_verifications () == verifications,
        "the verification began again after Escape:\n" .. mock_log ())
results.escape_stopped = true

test.log ("Choosing the first user again")
test.press_accelerator ("Return")
test.wait_for (function () return count_verifications () == verifications + 1 end,
               "the verification to begin again", TIMEOUT)
test.wait_leisure ()

test.log ("Typing the password of " .. results.verified_user)
type_text (PASSWORD)
test.press_accelerator ("Return")
wait_mock ("gdm%-password: " .. user_pattern .. " verified",
           "the password to be accepted")
results.password_accepted = true

wait_mock ("the session would start now", "the session to start")
results.session_started = true
