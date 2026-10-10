#pragma once

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/**
 * The connected MOUSE (@p keyboard false) or KEYBOARD's own name, as the device reports it -- e.g.
 * "G502 HERO Gaming Mouse" -- for the co-op controller routing, which lists the mouse and keyboard
 * beside the controllers (user 2026-09-28: "It should use the actual mouse and keyboard's name if
 * possible"). With several, the likeliest one is named (they all move the one cursor anyway).
 * False when there is none or it reports no name; the caller then shows "Mouse" / "Keyboard".
 * Only Windows can say; elsewhere the answer is always false.
 */
#ifdef _WIN32
bool input_device_name(bool keyboard, char *out, size_t len);
/**
 * Start noting which keyboard is TYPED ON (raw input, keyboard class only -- SDL2 registers mice
 * and pads, never keyboards). A mouse receiver often reports a keyboard too, and both of the
 * user's receivers report both, so only activity tells the keyboard apart (2026-09-28: both rows
 * read "USB Receiver"). Call once, after SDL video is up.
 */
void input_names_start(void);
#else
static inline bool input_device_name(bool keyboard, char *out, size_t len) {
    (void) keyboard;
    (void) out;
    (void) len;
    return false;
}
static inline void input_names_start(void) {}
#endif

/**
 * The co-op lists' label: "NAME (KIND)" -- a receiver's name alone ("USB Receiver") does not say
 * which of the two it is -- or KIND alone when the device reports no name. @p kind is the
 * caller's localized "Mouse" / "Keyboard"; it is not repeated when the name already says it.
 */
static inline void input_device_label(bool keyboard, const char *kind, char *out, size_t len) {
    char name[96];
    if (!input_device_name(keyboard, name, sizeof(name))) {
        snprintf(out, len, "%s", kind);
        return;
    }
    char lname[96], lkind[64];
    size_t i;
    for (i = 0; name[i] != '\0' && i < sizeof(lname) - 1; i++) {
        lname[i] = (char) tolower((unsigned char) name[i]);
    }
    lname[i] = '\0';
    for (i = 0; kind[i] != '\0' && i < sizeof(lkind) - 1; i++) {
        lkind[i] = (char) tolower((unsigned char) kind[i]);
    }
    lkind[i] = '\0';
    if (strstr(lname, lkind) != NULL) {
        snprintf(out, len, "%s", name);
    } else {
        snprintf(out, len, "%s (%s)", name, kind);
    }
}
