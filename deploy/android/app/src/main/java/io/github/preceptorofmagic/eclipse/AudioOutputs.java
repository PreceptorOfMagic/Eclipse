package io.github.preceptorofmagic.eclipse;

/**
 * Names for the audio outputs the settings offer (src/app/ui/settings/panes/audio.pane.c). Desktop systems
 * name their outputs and Eclipse stores that name; Android numbers them, and the numbers change across reboots
 * and reconnections. So the stored name is built from what stays: the kind of output and the product it leads
 * to ("HDMI: LG TV", "Bluetooth: WH-1000XM5"). The SS4S SDL module turns it back into the current number when
 * a stream opens its sound (EclipseActivity.getAudioOutputId).
 *
 * Plain Java with no Android types, so tests/android checks it on the host.
 */
final class AudioOutputs {
    private AudioOutputs() {
    }

    /** What kind of output an android.media.AudioDeviceInfo type is; null for one that does not play media. */
    static String kindOf(int type) {
        switch (type) {
            case 2:  /* TYPE_BUILTIN_SPEAKER */
                return "Speaker";
            case 3:  /* TYPE_WIRED_HEADSET */
                return "Headset";
            case 4:  /* TYPE_WIRED_HEADPHONES */
                return "Headphones";
            case 5:  /* TYPE_LINE_ANALOG */
                return "Line out";
            case 6:  /* TYPE_LINE_DIGITAL */
                return "Digital out";
            case 8:  /* TYPE_BLUETOOTH_A2DP */
                return "Bluetooth";
            case 9:  /* TYPE_HDMI */
                return "HDMI";
            case 10: /* TYPE_HDMI_ARC */
                return "HDMI ARC";
            case 11: /* TYPE_USB_DEVICE */
            case 12: /* TYPE_USB_ACCESSORY */
                return "USB";
            case 13: /* TYPE_DOCK */
            case 31: /* TYPE_DOCK_ANALOG */
                return "Dock";
            case 19: /* TYPE_AUX_LINE */
                return "Aux";
            case 22: /* TYPE_USB_HEADSET */
                return "USB headset";
            case 23: /* TYPE_HEARING_AID */
                return "Hearing aid";
            case 26: /* TYPE_BLE_HEADSET */
            case 27: /* TYPE_BLE_SPEAKER */
                return "Bluetooth LE";
            case 29: /* TYPE_HDMI_EARC */
                return "HDMI eARC";
            default: /* earpiece, telephony, SCO voice, bus, remote submix, echo reference, IP, tuners, unknown */
                return null;
        }
    }

    /**
     * The name of each output, in the order given (null for one that does not play media). Two outputs that
     * would share a name are numbered in that order: callers list them by ascending device id, so the numbering
     * follows the order Android created them in.
     */
    static String[] label(int[] types, CharSequence[] products) {
        String[] labels = new String[types.length];
        for (int i = 0; i < types.length; ++i) {
            String kind = kindOf(types[i]);
            if (kind == null) {
                continue;
            }
            String product = products[i] != null ? clean(products[i].toString()) : "";
            String base = product.isEmpty() ? kind : kind + ": " + product;
            String name = base;
            for (int n = 2; contains(labels, i, name); ++n) {
                name = base + " " + n;
            }
            labels[i] = name;
        }
        return labels;
    }

    /** One line per output, "id|name", for the native side (android_platform.c). */
    static String lines(int[] ids, String[] labels) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < ids.length; ++i) {
            if (labels[i] != null) {
                out.append(ids[i]).append('|').append(labels[i]).append('\n');
            }
        }
        return out.toString();
    }

    /** The id of the output called @p name, or 0 (Android's ids start at 1). */
    static int idOf(int[] ids, String[] labels, String name) {
        for (int i = 0; i < ids.length; ++i) {
            if (labels[i] != null && labels[i].equals(name)) {
                return ids[i];
            }
        }
        return 0;
    }

    /* The name travels in "id|name" lines and in moonlight.ini: one line, no separator, no outer spaces. */
    private static String clean(String s) {
        return s.replace('|', '/').replace('\n', ' ').replace('\r', ' ').trim();
    }

    private static boolean contains(String[] labels, int before, String name) {
        for (int i = 0; i < before; ++i) {
            if (name.equals(labels[i])) {
                return true;
            }
        }
        return false;
    }
}
