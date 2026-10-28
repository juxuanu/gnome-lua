-- Exits the shell while there are windows, one of them with text input.

test.wait_startup ()

-- Make created windows remain visible during exit.
test.disable_helper_auto_exit ()

test.create_virtual_device ("keyboard-device")

main.overview:hide ()
test.wait_leisure ()
test.sleep (1000)

test.create_window {
  width = 640,
  height = 480,
  text_input = true,
}

test.wait_windows ()
test.wait_leisure ()
test.sleep (1000)
