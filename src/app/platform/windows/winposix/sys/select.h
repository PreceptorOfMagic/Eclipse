/* Win32 stand-in for a POSIX header the shared sources include. select()/fd_set come from
 * winsock2.h, which coop_win_compat.h pulls in. */
#include "coop_win_compat.h"
