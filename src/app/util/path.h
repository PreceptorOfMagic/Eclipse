#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#if defined(_WIN32)
#define PATH_SEPARATOR '\\'
#else
#define PATH_SEPARATOR '/'
#endif

char *path_join(const char *parent, const char *basename);

void path_join_to(char *dest, size_t maxlen, const char *parent, const char *basename);

char *path_assets();

char *path_pref(bool *persistent);

char *path_cache();

/** Cache for release runtime state; refuses the platform's shared temporary fallback. */
char *path_runtime_cache(void);

/** Per-user diagnostic locations used by desktop builds. The returned value is
 * thread-local and remains valid until the next path_*_log call on that thread. */
const char *path_diag_log(void);
const char *path_secondary_log(void);
const char *path_multiview_log(void);

/**
 * Absolute path of the currently running executable.
 *
 * The caller supplies storage so this is safe on the video setup path and does not
 * introduce another ownership convention. Returns true only when the complete path
 * fits in @p dest.
 */
bool path_self_executable(char *dest, size_t maxlen);

/** Allocating form of path_self_executable(). The caller owns the result. */
char *path_self_executable_alloc(void);

int path_dir_ensure(const char *dir);

/**
 * Open a UTF-8 path on every platform.
 *
 * Windows' narrow CRT file APIs interpret paths in the process ANSI code page,
 * while SDL returns its preference path as UTF-8.  Keeping the conversion at
 * this boundary prevents every settings/credential caller from growing its own
 * (usually incomplete) Windows special case.  On POSIX this is fopen().
 */
FILE *path_fopen(const char *path, const char *mode);

/** Reopen an existing stdio stream using a UTF-8 path. This is the portable
 * equivalent of freopen(), including UTF-16 conversion on Windows. */
FILE *path_freopen(const char *path, const char *mode, FILE *stream);

/** Return true when a regular file can be opened for reading through the UTF-8
 * path boundary. */
bool path_file_readable(const char *path);

/** Copy a file through the UTF-8 path boundary. A failed copy removes the
 * incomplete destination. */
int path_file_copy(const char *source, const char *target);

/** UTF-8 equivalents of remove() and an atomic replace of one file by another. */
int path_file_remove(const char *path);
bool path_file_replace(const char *source, const char *target);
