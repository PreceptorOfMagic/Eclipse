/* Win32 stand-in for a POSIX header the shared sources include. Everything real lives in
 * coop_win_compat.h, which the build force-includes for these translation units only;
 * this file exists so the #include line resolves and the source stays unchanged. */
#include "coop_win_compat.h"
