package io.github.preceptorofmagic.eclipse;

/**
 * The TV remote keys SDL 2 leaves unmapped, as the webOS remote's equivalents (platform/android/keyboard_android.c):
 * EclipseActivity hands them to the native side as these numbers (android_platform.h ANDROID_SCANCODE_*).
 *
 * Plain Java with no Android types, so tests/android checks it on the host. Key codes are android.view.KeyEvent's.
 */
final class TvKeys {
    /* Must match ANDROID_SCANCODE_* - ANDROID_SCANCODE_TV_BASE in android_platform.h. */
    static final int RED = 1;
    static final int GREEN = 2;
    static final int YELLOW = 3;
    static final int BLUE = 4;
    static final int CH_UP = 5;
    static final int CH_DOWN = 6;
    static final int OVERLAY = 7;
    static final int MENU = 8;

    /* android.view.KeyEvent key codes */
    static final int KEYCODE_MENU = 82;
    static final int KEYCODE_CHANNEL_UP = 166;
    static final int KEYCODE_CHANNEL_DOWN = 167;
    static final int KEYCODE_INFO = 165;
    static final int KEYCODE_GUIDE = 172;
    static final int KEYCODE_PROG_RED = 183;
    static final int KEYCODE_PROG_GREEN = 184;
    static final int KEYCODE_PROG_YELLOW = 185;
    static final int KEYCODE_PROG_BLUE = 186;

    private TvKeys() {
    }

    /**
     * The TV key for @p keyCode, 0 for one SDL handles itself. A controller's buttons are always SDL's; a full
     * keyboard's Menu key stays the host's Applications key, as on a PC.
     */
    static int forKeyCode(int keyCode, boolean fromController, boolean fromAlphabeticKeyboard) {
        if (fromController) {
            return 0;
        }
        switch (keyCode) {
            case KEYCODE_PROG_RED:
                return RED;
            case KEYCODE_PROG_GREEN:
                return GREEN;
            case KEYCODE_PROG_YELLOW:
                return YELLOW;
            case KEYCODE_PROG_BLUE:
                return BLUE;
            case KEYCODE_CHANNEL_UP:
                return CH_UP;
            case KEYCODE_CHANNEL_DOWN:
                return CH_DOWN;
            case KEYCODE_INFO:
            case KEYCODE_GUIDE:
                return OVERLAY;
            case KEYCODE_MENU:
                return fromAlphabeticKeyboard ? 0 : MENU;
            default:
                return 0;
        }
    }
}
