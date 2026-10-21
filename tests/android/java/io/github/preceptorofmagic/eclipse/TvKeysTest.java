package io.github.preceptorofmagic.eclipse;

/* The TV remote keys EclipseActivity hands to the native side (deploy/android/.../TvKeys.java), on the host JDK. */
public final class TvKeysTest {
    private static int failures;

    private static void expect(int got, int want, String what) {
        if (got != want) {
            System.out.println("FAIL: " + what + ": wanted " + want + ", got " + got);
            failures++;
        }
    }

    public static void main(String[] args) {
        expect(TvKeys.forKeyCode(TvKeys.KEYCODE_PROG_RED, false, false), TvKeys.RED, "red");
        expect(TvKeys.forKeyCode(TvKeys.KEYCODE_PROG_GREEN, false, false), TvKeys.GREEN, "green");
        expect(TvKeys.forKeyCode(TvKeys.KEYCODE_PROG_YELLOW, false, false), TvKeys.YELLOW, "yellow");
        expect(TvKeys.forKeyCode(TvKeys.KEYCODE_PROG_BLUE, false, false), TvKeys.BLUE, "blue");
        expect(TvKeys.forKeyCode(TvKeys.KEYCODE_CHANNEL_UP, false, false), TvKeys.CH_UP, "channel up");
        expect(TvKeys.forKeyCode(TvKeys.KEYCODE_CHANNEL_DOWN, false, false), TvKeys.CH_DOWN, "channel down");
        expect(TvKeys.forKeyCode(TvKeys.KEYCODE_INFO, false, false), TvKeys.OVERLAY, "info");
        expect(TvKeys.forKeyCode(TvKeys.KEYCODE_GUIDE, false, false), TvKeys.OVERLAY, "guide");
        expect(TvKeys.forKeyCode(TvKeys.KEYCODE_MENU, false, false), TvKeys.MENU, "a remote's menu");
        expect(TvKeys.forKeyCode(TvKeys.KEYCODE_MENU, false, true), 0, "a full keyboard's menu: the host's Apps key");
        expect(TvKeys.forKeyCode(TvKeys.KEYCODE_PROG_RED, true, false), 0, "a controller's buttons are SDL's");
        expect(TvKeys.forKeyCode(TvKeys.KEYCODE_MENU, true, false), 0, "a controller's menu (start) is SDL's");
        expect(TvKeys.forKeyCode(4 /* BACK */, false, false), 0, "Back is SDL's (AC_BACK)");
        expect(TvKeys.forKeyCode(23 /* DPAD_CENTER */, false, false), 0, "OK is SDL's (SELECT)");
        expect(TvKeys.forKeyCode(19 /* DPAD_UP */, false, false), 0, "the d-pad is SDL's");
        /* The numbers are android.view.KeyEvent's: a typo here would silently never match a real remote. */
        expect(TvKeys.KEYCODE_PROG_RED, 183, "KEYCODE_PROG_RED");
        expect(TvKeys.KEYCODE_PROG_BLUE, 186, "KEYCODE_PROG_BLUE");
        expect(TvKeys.KEYCODE_CHANNEL_UP, 166, "KEYCODE_CHANNEL_UP");
        expect(TvKeys.KEYCODE_INFO, 165, "KEYCODE_INFO");
        expect(TvKeys.KEYCODE_GUIDE, 172, "KEYCODE_GUIDE");
        expect(TvKeys.KEYCODE_MENU, 82, "KEYCODE_MENU");
        /* The SHIELD TV turns a Switch Pro Controller's + into Play/Pause: from a controller it is that controller's
         * Start, unless a real Start came first (then it is the copy); a remote's Play/Pause stays a media key. */
        final int pp = TvKeys.KEYCODE_MEDIA_PLAY_PAUSE;
        expect(TvKeys.controllerKey(pp, true, true, 10_000), TvKeys.CONTROLLER_KEY_AS_START, "controller Play/Pause = Start");
        expect(TvKeys.controllerKey(pp, true, true, 5), TvKeys.CONTROLLER_KEY_DROP, "Play/Pause right after a real Start");
        expect(TvKeys.controllerKey(pp, false, true, 10_000), TvKeys.CONTROLLER_KEY_KEEP, "a remote's Play/Pause");
        expect(TvKeys.controllerKey(pp, true, false, 10_000), TvKeys.CONTROLLER_KEY_KEEP, "a controller with no Start");
        expect(TvKeys.controllerKey(TvKeys.KEYCODE_BUTTON_START, true, true, 0), TvKeys.CONTROLLER_KEY_KEEP,
               "a real Start passes");
        expect(TvKeys.KEYCODE_MEDIA_PLAY_PAUSE, 85, "KEYCODE_MEDIA_PLAY_PAUSE");
        expect(TvKeys.KEYCODE_BUTTON_START, 108, "KEYCODE_BUTTON_START");
        if (failures == 0) {
            System.out.println("TvKeys: OK");
        }
        System.exit(failures == 0 ? 0 : 1);
    }
}
