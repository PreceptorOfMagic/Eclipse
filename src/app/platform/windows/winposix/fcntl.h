/* Win32 stand-in for a POSIX header the shared co-op sources include.
 * Everything real lives in coop_win_compat.h, which the build force-includes;
 * this file exists only so those #include lines resolve, keeping the sources
 * byte-identical to the webOS client's copies. */
#include "coop_win_compat.h"
