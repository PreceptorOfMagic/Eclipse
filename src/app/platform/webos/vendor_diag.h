#pragma once

/* Vendor media diagnostics -- see vendor_diag.c. Compiled in only with
 * -DAURORA_VENDOR_DIAG=ON (webOS); every other build gets these no-op stubs, so call
 * sites need no #ifdef. */
#if defined(AURORA_VENDOR_DIAG) && AURORA_VENDOR_DIAG

void vendor_diag_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void vendor_diag_hang_arm(const char *label);

void vendor_diag_hang_disarm(const char *result);

#else

static inline void vendor_diag_note(const char *fmt, ...) { (void) fmt; }

static inline void vendor_diag_hang_arm(const char *label) { (void) label; }

static inline void vendor_diag_hang_disarm(const char *result) { (void) result; }

#endif
