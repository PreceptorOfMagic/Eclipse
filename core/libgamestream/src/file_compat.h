#pragma once

#include <stddef.h>
#include <stdio.h>

/* All public paths in libgamestream are UTF-8.  POSIX already has that byte-path
 * convention; Windows requires an explicit UTF-16 boundary for non-ACP profiles. */
FILE *gs_fopen_utf8(const char *path, const char *mode);

/* POSIX: require an owned, single-link regular file and enforce 0600 before
 * reading or writing. Refuse symlinks; writing truncates only after validation.
 * A readable foreign-owned legacy key is copied and atomically replaced with an
 * owned 0600 file on read if its parent permits it; key bytes are preserved.
 * Windows retains the existing UTF-16/profile ACL boundary. */
FILE *gs_fopen_private_utf8(const char *path, int writing);

/* Caller owns returned storage. */
char *gs_path_join(const char *parent, const char *basename);

/* Create @directory and all missing parents. */
int gs_mkdirs_utf8(const char *directory);

/* Read an entire file and append a NUL byte not included in size_out. */
int gs_read_file_utf8(const char *path, unsigned char **data_out, size_t *size_out);
int gs_read_private_file_utf8(const char *path, unsigned char **data_out, size_t *size_out);
