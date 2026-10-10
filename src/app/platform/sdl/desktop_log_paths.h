#pragma once
/* Linux desktop twin of the log-path half of platform/windows/win_posix_bits.h: the co-op
 * diagnostics resolve under the user's SDL preference directory (path_sdl.c), exactly as on
 * Windows, instead of the TV's fixed /tmp names -- which CMake defines only for webOS. */
const char *path_diag_log(void);
const char *path_secondary_log(void);
const char *path_multiview_log(void);

#ifndef AURORA_DIAG_LOG
#define AURORA_DIAG_LOG path_diag_log()
#endif
#ifndef AURORA_SECONDARY_LOG
#define AURORA_SECONDARY_LOG path_secondary_log()
#endif
#ifndef AURORA_MULTIVIEW_LOG
#define AURORA_MULTIVIEW_LOG path_multiview_log()
#endif
