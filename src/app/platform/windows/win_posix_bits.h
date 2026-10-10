#pragma once
/*
 * THE SAFE HALF OF THE POSIX SHIM, force-included for every Windows translation unit.
 *
 * Deliberately NOT coop_win_compat.h: that one remaps close/read/write/poll/fcntl onto
 * their winsock equivalents, which is correct for a socket source and wrong for everything
 * else -- force-including it app-wide would rewrite ordinary file calls. Only additions
 * that cannot change the meaning of existing code live here.
 */
#ifdef _WIN32

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Diagnostic files belong to the user's application directory, not whatever
 * working directory happened to launch a shortcut. path_sdl.c owns the actual
 * platform path lookup; these function-valued macros preserve the existing
 * fopen/stat call sites without baking a profile name into the binary. */
const char *path_diag_log(void);
const char *path_secondary_log(void);
const char *path_multiview_log(void);
FILE *path_fopen(const char *path, const char *mode);

#ifndef AURORA_DIAG_LOG
#define AURORA_DIAG_LOG path_diag_log()
#endif
#ifndef AURORA_SECONDARY_LOG
#define AURORA_SECONDARY_LOG path_secondary_log()
#endif
#ifndef AURORA_MULTIVIEW_LOG
#define AURORA_MULTIVIEW_LOG path_multiview_log()
#endif

/* App paths originate in SDL/Win32 UTF-8 APIs.  The narrow CRT fopen instead
 * interprets them in the active ANSI code page, so a perfectly normal Unicode
 * profile name loses settings, host records and diagnostics.  stdio is included
 * above before this remap, and path.c implements the boundary with _wfopen(). */
#define fopen(path, mode) path_fopen((path), (mode))

/* setenv/unsetenv are POSIX; the CRT has _putenv_s with different semantics for removal. */
static inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name) != NULL) { return 0; }
    return _putenv_s(name, value == NULL ? "" : value);
}

static inline int unsetenv(const char *name) { return _putenv_s(name, ""); }

/* inet_aton is the old BSD spelling; winsock has inet_addr. A MACRO, not a function,
 * deliberately: this header is force-included before anything else, so a function body
 * mentioning inet_addr/INADDR_NONE would declare them implicitly and then collide with
 * winsock's real declarations. As a macro it is expanded at the call site, where winsock
 * has been included. Keeps inet_aton's contract, including the 255.255.255.255 case that
 * inet_addr cannot distinguish from failure. */
#define inet_aton(cp, addr) \
    ((*(unsigned long *) (addr) = inet_addr(cp)), \
     (*(unsigned long *) (addr) != INADDR_NONE || strcmp((cp), "255.255.255.255") == 0))

#endif /* _WIN32 */
