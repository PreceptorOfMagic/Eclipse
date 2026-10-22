#pragma once
/* TEST-ONLY: the key codes src/app/input/input_gamepad_mapping.c uses, so the Android type-check (tests/android)
 * can compile it on a desktop. Values are the Android NDK's (android/keycodes.h = android.view.KeyEvent). */
enum {
    AKEYCODE_MENU = 82,
    AKEYCODE_BACK = 4,
    AKEYCODE_DPAD_UP = 19,
    AKEYCODE_DPAD_DOWN = 20,
    AKEYCODE_DPAD_LEFT = 21,
    AKEYCODE_DPAD_RIGHT = 22,
    AKEYCODE_DPAD_CENTER = 23,
    AKEYCODE_BUTTON_A = 96,
    AKEYCODE_BUTTON_B = 97,
    AKEYCODE_BUTTON_C = 98,
    AKEYCODE_BUTTON_X = 99,
    AKEYCODE_BUTTON_Y = 100,
    AKEYCODE_BUTTON_Z = 101,
    AKEYCODE_BUTTON_L1 = 102,
    AKEYCODE_BUTTON_R1 = 103,
    AKEYCODE_BUTTON_L2 = 104,
    AKEYCODE_BUTTON_R2 = 105,
    AKEYCODE_BUTTON_THUMBL = 106,
    AKEYCODE_BUTTON_THUMBR = 107,
    AKEYCODE_BUTTON_START = 108,
    AKEYCODE_BUTTON_SELECT = 109,
    AKEYCODE_BUTTON_MODE = 110,
    AKEYCODE_BUTTON_1 = 188,
};
