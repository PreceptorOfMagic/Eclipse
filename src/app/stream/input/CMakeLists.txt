target_sources(moonlight-lib PRIVATE
        keyboard_input.c
        session_input.c
        session_keyboard.c
        session_gamepad.c coop_input.c coop_gamepad_map.c coop_assign.c coop_pi_pads.c coop_pointer.c
        session_mouse.c
        session_pointer_gesture.c
        session_touch.c
        session_virt_mouse.c
        start_wheel.c)
if (FEATURE_INPUT_EVMOUSE)
    target_sources(moonlight-lib PRIVATE session_evmouse.c)
endif ()
