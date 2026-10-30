-- This script tests the login screen (src/gdm/login-dialog.c,
-- src/gdm/user-verifier.c): the shell runs as GDM's greeter, against
-- tests/greeter-mock.py, which stands in for GDM and AccountsService and
-- logs what it is asked to GREETER_MOCK_LOG. Enter on the user list picks
-- the first user: GDM must then be asked to verify that user (no user of the
-- mock has a session to reauthenticate, as at a real login screen), not a
-- user to be named. Typing the password logs in.

local TIMEOUT = 10000
local PASSWORD = "gnome"

local mock_log_path = assert (os.getenv ("GREETER_MOCK_LOG"),
                              "GREETER_MOCK_LOG is not set: run under gnomeshell_dbusrunner.py")

local results = {
  verified_user = nil,
  asked_for_username = false,
  password_accepted = false,
  session_started = false,
}

test.finish (function ()
  assert (not results.asked_for_username,
          "Choosing a user asked for the username")
  assert (results.verified_user ~= nil, "Choosing a user did not begin its verification")
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

test.log ("Typing the password of " .. results.verified_user)
type_text (PASSWORD)
test.press_accelerator ("Return")
wait_mock ("gdm%-password: " .. results.verified_user:gsub ("%p", "%%%0") .. " verified",
           "the password to be accepted")
results.password_accepted = true

wait_mock ("the session would start now", "the session to start")
results.session_started = true
