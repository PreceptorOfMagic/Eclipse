#pragma once

/* Include the native declarations before the narrowly scoped call adapters. */
#include <stdio.h>
#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(ECLIPSE_POSIX_RUNTIME) && !defined(AURORA_TEST_HOOKS) && !defined(AURORA_VENDOR_DIAG)
#define ECLIPSE_RUNTIME_ACTIVE 1
#ifdef __cplusplus
extern "C" {
#endif
FILE *eclipse_runtime_fopen(const char *path, const char *mode);
int eclipse_runtime_access(const char *path, int mode);
int eclipse_runtime_unlink(const char *path);
int eclipse_runtime_stat(const char *path, struct stat *out);
/* Copies a short descriptor-relative alias for a virtual socket pathname.
 * The process holds the directory until exit; an exec'd secondary opens its own.
 * Returns -1 on refusal, never a shared /tmp fallback. */
int eclipse_runtime_socket_path(const char *path, char *out, size_t size);
#ifdef __cplusplus
}
#endif
#define fopen(path, mode) eclipse_runtime_fopen((path), (mode))
#define access(path, mode) eclipse_runtime_access((path), (mode))
#define unlink(path) eclipse_runtime_unlink(path)
#define stat(path, out) eclipse_runtime_stat((path), (out))
#endif
