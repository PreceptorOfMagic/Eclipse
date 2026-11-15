#pragma once

#include <stdbool.h>

/**
 * The next tip: the most important one this device has not shown yet (hints_core.h), and it is remembered as
 * shown. The stream screen and the settings screen share one rotation. @p coop / @p hdr let co-op and HDR tips
 * come first on those streams (the settings screen passes false for both). NULL when there is none.
 */
const char *hints_obtain(bool coop, bool hdr);

/** The settings bar's next tip: every tip is eligible there, co-op and HDR included, in the settings-bar order;
 *  shares the history with hints_obtain(). NULL when there is none. */
const char *hints_obtain_settings(void);

/** Every tip this platform shows, grouped and numbered in order of importance, for the "All tips" dialog.
 *  Marks nothing as shown. Caller frees with free(); NULL if out of memory. */
char *hints_all_text(void);
