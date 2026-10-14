package io.github.preceptorofmagic.eclipse;

/* The display mode a stream gets on Android (deploy/android/.../DisplayModes.java), on the host JDK. */
public final class DisplayModesTest {
    private static int failures;

    private static void expect(int[] rates, int want, int expectedRate, String what) {
        int i = DisplayModes.pick(rates, want);
        int got = i >= 0 ? rates[i] : -1;
        if (got != expectedRate) {
            System.out.println("FAIL: " + what + ": wanted " + expectedRate + ", picked " + got);
            failures++;
        }
    }

    public static void main(String[] args) {
        int[] tv = {2398, 2400, 2500, 2997, 3000, 5000, 5994, 6000};   /* a 4K TV's modes at 2160p */
        expect(tv, 6000, 6000, "60 FPS: the 60 Hz mode, not 59.94");
        expect(tv, 5994, 5994, "59.94: the 59.94 Hz mode");
        expect(tv, 2398, 2398, "23.976");
        expect(tv, 12000, 6000, "120 FPS on a 60 Hz TV: its highest");

        int[] noThirty = {5000, 5994, 6000};
        expect(noThirty, 3000, 6000, "30 FPS without a 30 Hz mode: 60 (each picture twice), never the nearer 50");
        expect(noThirty, 2997, 5994, "29.97 without a 29.97 mode: 59.94");
        expect(noThirty, 4000, 6000, "a rate nothing divides evenly: the highest");

        int[] gaming = {6000, 12000};
        expect(gaming, 12000, 12000, "120 Hz TV, 120 FPS");
        expect(gaming, 4000, 12000, "40 FPS on a 120 Hz TV: 120 (x3)");
        expect(gaming, 3000, 6000, "30 FPS: the smallest multiple, 60");
        expect(new int[]{11988, 5994}, 5994, 5994, "NTSC 120 Hz TV, 59.94");
        expect(new int[0], 6000, -1, "no modes");

        check(DisplayModes.isMultiple(5994, 2997), "59.94 is 2 x 29.97");
        check(!DisplayModes.isMultiple(6000, 2997), "60 is not 2 x 29.97");
        check(DisplayModes.isMultiple(11988, 5994), "119.88 is 2 x 59.94");
        if (failures == 0) {
            System.out.println("DisplayModes: OK");
        }
        System.exit(failures == 0 ? 0 : 1);
    }

    private static void check(boolean ok, String what) {
        if (!ok) {
            System.out.println("FAIL: " + what);
            failures++;
        }
    }
}
