package io.github.preceptorofmagic.eclipse;

/* The names the Android audio settings store (deploy/android/.../AudioOutputs.java), checked on the host JDK. */
public final class AudioOutputsTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        if (!ok) {
            System.out.println("FAIL: " + what);
            failures++;
        }
    }

    public static void main(String[] args) {
        /* A SHIELD on a TV with a soundbar on ARC, Bluetooth headphones, its own earpiece-less telephony
         * endpoint, a remote submix and two identical USB DACs. */
        int[] ids = {3, 5, 9, 12, 14, 20, 21};
        int[] types = {9, 10, 8, 18, 25, 11, 11};
        CharSequence[] products = {"LG TV", "Sonos Arc", "WH-1000XM5", "SHIELD", "", "DAC|X\n", "DAC|X\n"};
        String[] labels = AudioOutputs.label(types, products);
        check("HDMI: LG TV".equals(labels[0]), "HDMI named by the TV: " + labels[0]);
        check("HDMI ARC: Sonos Arc".equals(labels[1]), "ARC: " + labels[1]);
        check("Bluetooth: WH-1000XM5".equals(labels[2]), "Bluetooth: " + labels[2]);
        check(labels[3] == null, "telephony is not a media output");
        check(labels[4] == null, "remote submix is not a media output");
        check("USB: DAC/X".equals(labels[5]), "separator and newline cleaned: " + labels[5]);
        check("USB: DAC/X 2".equals(labels[6]), "second identical device numbered: " + labels[6]);

        String lines = AudioOutputs.lines(ids, labels);
        check(lines.equals("3|HDMI: LG TV\n5|HDMI ARC: Sonos Arc\n9|Bluetooth: WH-1000XM5\n20|USB: DAC/X\n21|USB: DAC/X 2\n"),
                "lines: " + lines);
        check(AudioOutputs.idOf(ids, labels, "Bluetooth: WH-1000XM5") == 9, "lookup by name");
        check(AudioOutputs.idOf(ids, labels, "USB: DAC/X 2") == 21, "lookup of the numbered duplicate");
        check(AudioOutputs.idOf(ids, labels, "Bluetooth: gone") == 0, "a disconnected output is 0");

        /* After a reboot the same devices get new ids: the names do not change. */
        int[] ids2 = {40, 41, 44};
        String[] after = AudioOutputs.label(new int[]{9, 10, 8}, new CharSequence[]{"LG TV", "Sonos Arc", "WH-1000XM5"});
        check(AudioOutputs.idOf(ids2, after, "HDMI: LG TV") == 40, "same name, new id after reboot");

        /* No product name: the kind alone. Every media type has a kind; voice-only ones do not. */
        check("Speaker".equals(AudioOutputs.label(new int[]{2}, new CharSequence[]{null})[0]), "speaker without product");
        for (int t : new int[]{2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 19, 22, 23, 26, 27, 29, 31}) {
            check(AudioOutputs.kindOf(t) != null, "media output type " + t);
        }
        for (int t : new int[]{0, 1, 7, 15, 18, 20, 21, 24, 25, 28}) {
            check(AudioOutputs.kindOf(t) == null, "non-media type " + t);
        }
        if (failures == 0) {
            System.out.println("AudioOutputs: OK");
        }
        System.exit(failures == 0 ? 0 : 1);
    }
}
