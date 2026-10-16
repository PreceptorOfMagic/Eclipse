package io.github.preceptorofmagic.eclipse;

/**
 * Which of the TV's refresh rates (at its current resolution) shows a stream best. The webOS panel does this
 * itself; on Android the activity asks for a display mode (EclipseActivity.requestRefreshRate). Rates are in
 * hundredths of a Hz, as Limelight's clientRefreshRateX100.
 *
 * Plain Java with no Android types, so tests/android checks it on the host.
 */
final class DisplayModes {
    /** How close two rates must be to count as the same: rounding to hundredths only. 23.976 (2398) is not 24,
     *  59.94 is not 60. */
    static final int TOLERANCE_X100 = 1;

    private DisplayModes() {
    }

    /**
     * The index in @p ratesX100 of the mode for a stream at @p wantX100, -1 if there are none:
     * the same rate; else the smallest whole multiple of it (a 30 FPS stream on 60 Hz shows every picture twice,
     * evenly); else the highest rate there is (the TV's least latency, as when nothing matches on any client).
     */
    static int pick(int[] ratesX100, int wantX100) {
        int same = -1, multiple = -1, highest = -1;
        for (int i = 0; i < ratesX100.length; ++i) {
            int rate = ratesX100[i];
            int diff = Math.abs(rate - wantX100);
            if (diff <= TOLERANCE_X100) {
                if (same < 0 || diff < Math.abs(ratesX100[same] - wantX100)) {
                    same = i;
                }
            } else if (wantX100 > 0 && rate > wantX100 && isMultiple(rate, wantX100) &&
                    (multiple < 0 || rate < ratesX100[multiple])) {
                multiple = i;
            }
            if (highest < 0 || rate > ratesX100[highest]) {
                highest = i;
            }
        }
        return same >= 0 ? same : multiple >= 0 ? multiple : highest;
    }

    static boolean isMultiple(int rateX100, int baseX100) {
        int n = Math.round((float) rateX100 / baseX100);
        return n >= 1 && Math.abs(rateX100 - n * baseX100) <= TOLERANCE_X100 * n;
    }
}
