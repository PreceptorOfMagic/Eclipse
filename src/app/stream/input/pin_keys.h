#pragma once

#include <stdbool.h>

/*
 * ASCII -> Windows virtual-key code, for typing a Wake-on-LAN sign-in credential at
 * the Windows lock screen.
 *
 * WHY THIS EXISTS: the auto-entry used to type digits only (`VK_0 + digit`), which is
 * all a PIN needs. A PASSWORD is any character, and a password half-typed -- letters
 * dropped, symbols missing -- fails to sign in while looking, from the TV, exactly
 * like a wrong password. There is no feedback channel, so the mapping has to be right
 * rather than best-effort: anything this cannot type is REFUSED by the caller instead
 * of being silently skipped.
 *
 * US LAYOUT, deliberately. Moonlight carries VK codes, and a VK code means "the key in
 * that position", so the character it produces is decided by the layout selected on
 * the HOST. This maps the US layout because that is what the codes are named for; a
 * host on another layout would need its own table, which is a real limitation and is
 * better stated here than discovered at a lock screen.
 *
 * Shared by the primary's pin_autoentry.c and the headless co-op secondary, which do
 * the same job on two different connections.
 */

#define PIN_VK_RETURN 0x0D
#define PIN_VK_SHIFT  0x10
#define PIN_VK_SPACE  0x20
#define PIN_VK_0      0x30

/**
 * @param c     character to type.
 * @param vk    receives the virtual-key code.
 * @param shift receives whether Shift must be held for it.
 * @return false when @p c cannot be typed on a US layout, so the caller can refuse the
 *         credential rather than type a mangled one.
 */
static inline bool pin_key_for_char(char c, short *vk, bool *shift) {
    *shift = false;
    if (c >= '0' && c <= '9') { *vk = (short) (PIN_VK_0 + (c - '0')); return true; }
    if (c >= 'a' && c <= 'z') { *vk = (short) (0x41 + (c - 'a')); return true; }
    if (c >= 'A' && c <= 'Z') { *vk = (short) (0x41 + (c - 'A')); *shift = true; return true; }
    if (c == ' ') { *vk = PIN_VK_SPACE; return true; }

    /* Shifted digits. */
    static const char shifted_digits[] = ")!@#$%^&*(";
    for (int i = 0; i < 10; ++i) {
        if (c == shifted_digits[i]) { *vk = (short) (PIN_VK_0 + i); *shift = true; return true; }
    }

    /* OEM keys: unshifted char, shifted char, VK. */
    static const struct { char plain, shifted; short vk; } oem[] = {
            {';', ':',  0xBA},  /* VK_OEM_1 */
            {'=', '+',  0xBB},  /* VK_OEM_PLUS */
            {',', '<',  0xBC},  /* VK_OEM_COMMA */
            {'-', '_',  0xBD},  /* VK_OEM_MINUS */
            {'.', '>',  0xBE},  /* VK_OEM_PERIOD */
            {'/', '?',  0xBF},  /* VK_OEM_2 */
            {'`', '~',  0xC0},  /* VK_OEM_3 */
            {'[', '{',  0xDB},  /* VK_OEM_4 */
            {'\\', '|', 0xDC},  /* VK_OEM_5 */
            {']', '}',  0xDD},  /* VK_OEM_6 */
            {'\'', '"', 0xDE},  /* VK_OEM_7 */
    };
    for (unsigned i = 0; i < sizeof(oem) / sizeof(oem[0]); ++i) {
        if (c == oem[i].plain) { *vk = oem[i].vk; return true; }
        if (c == oem[i].shifted) { *vk = oem[i].vk; *shift = true; return true; }
    }
    return false;
}
