#include "controller_mapping_android.h"

#include <SDL_hints.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUID_LEN 32

/* Fields of a mapping that are not bindings. */
static bool is_condition_key(const char *key, size_t len) {
    static const char *const keys[] = {"platform", "hint", "sdk>=", "sdk<=", "crc", "type", "face"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strlen(keys[i]) == len && strncmp(key, keys[i], len) == 0) {
            return true;
        }
    }
    return false;
}

/* "b12" -> 12; -1 if the source is not a button. */
static int button_source(const char *value, size_t len) {
    if (len < 2 || value[0] != 'b') {
        return -1;
    }
    int n = 0;
    for (size_t i = 1; i < len; i++) {
        if (!isdigit((unsigned char) value[i]) || n > 1000) {
            return -1;
        }
        n = n * 10 + (value[i] - '0');
    }
    return n;
}

static bool is_hat_source(const char *value, size_t len) {
    return len >= 4 && value[0] == 'h' && isdigit((unsigned char) value[1]) && memchr(value, '.', len) != NULL;
}

controller_mapping_fit_t controller_mapping_fit(const char *mapping, controller_buttons_t buttons,
                                                controller_buttons_t known) {
    controller_mapping_fit_t fit = {0, 0};
    controller_buttons_t bound = 0;
    bool left_trigger = false, right_trigger = false;
    /* Bindings start after the GUID and the name. */
    const char *p = mapping != NULL ? strchr(mapping, ',') : NULL;
    p = p != NULL ? strchr(p + 1, ',') : NULL;
    while (p != NULL && *p != '\0') {
        const char *field = *p == ',' ? p + 1 : p;
        const char *end = strchr(field, ',');
        const size_t field_len = end != NULL ? (size_t) (end - field) : strlen(field);
        p = end;
        const char *colon = memchr(field, ':', field_len);
        if (colon == NULL) {
            continue;
        }
        const char *key = field;
        size_t key_len = (size_t) (colon - field);
        if (is_condition_key(key, key_len)) {
            continue;
        }
        if (key_len > 0 && (key[0] == '+' || key[0] == '-')) {
            key++;
            key_len--;
        }
        if (key_len == 11 && strncmp(key, "lefttrigger", 11) == 0) {
            left_trigger = true;
        } else if (key_len == 12 && strncmp(key, "righttrigger", 12) == 0) {
            right_trigger = true;
        }
        const char *value = colon + 1;
        size_t value_len = field_len - key_len - 1 - (size_t) (key - field);
        if (value_len > 0 && (value[0] == '+' || value[0] == '-')) {
            value++;
            value_len--;
        }
        if (is_hat_source(value, value_len)) {
            fit.dead++;
            continue;
        }
        const int button = button_source(value, value_len);
        if (button < 0) {
            continue;
        }
        if (button >= 64) {
            fit.dead++;
            continue;
        }
        const controller_buttons_t bit = (controller_buttons_t) 1 << button;
        bound |= bit;
        if ((known & bit) && !(buttons & bit)) {
            fit.dead++;
        }
    }
    /* L2 and R2 keys come with analog triggers on many controllers: a trigger bound to its axis uses them. */
    if (left_trigger) {
        bound |= (controller_buttons_t) 1 << 15;
    }
    if (right_trigger) {
        bound |= (controller_buttons_t) 1 << 16;
    }
    for (controller_buttons_t unbound = buttons & known & ~bound; unbound != 0; unbound &= unbound - 1) {
        fit.unused++;
    }
    return fit;
}

int controller_mapping_choose(const controller_mapping_candidate_t *candidates, int count,
                              controller_buttons_t buttons, controller_buttons_t known,
                              controller_mapping_fit_t *fits) {
    int best = -1;
    controller_mapping_fit_t best_fit = {0, 0};
    for (int i = 0; i < count; i++) {
        const controller_mapping_fit_t fit = controller_mapping_fit(candidates[i].mapping, buttons, known);
        if (fits != NULL) {
            fits[i] = fit;
        }
        if (best < 0 || fit.dead < best_fit.dead ||
            (fit.dead == best_fit.dead && fit.unused < best_fit.unused) ||
            (fit.dead == best_fit.dead && fit.unused == best_fit.unused &&
             candidates[i].source < candidates[best].source)) {
            best = i;
            best_fit = fit;
        }
    }
    return best;
}

static bool is_guid(const char *text) {
    for (int i = 0; i < GUID_LEN; i++) {
        if (!isxdigit((unsigned char) text[i])) {
            return false;
        }
    }
    return true;
}

/* Little-endian 16-bit word @p word of a GUID string (bytes 2*word and 2*word + 1). */
static unsigned guid_word(const char *guid, int word) {
    char lo[3] = {guid[word * 4], guid[word * 4 + 1], 0}, hi[3] = {guid[word * 4 + 2], guid[word * 4 + 3], 0};
    return (unsigned) strtoul(lo, NULL, 16) | (unsigned) strtoul(hi, NULL, 16) << 8;
}

/* SDL_CreateJoystickGUID's words: 0 bus, 1 CRC, 2 vendor, 4 product, 6 version -- which an Android GUID uses
 * for the button mask -- and 7 the axis mask (SDL 2.32.10 Android_AddJoystick). */
#define GUID_WORD_CRC 1
#define GUID_WORD_VENDOR 2
#define GUID_WORD_PRODUCT 4
#define GUID_WORD_BUTTONS 6
#define GUID_WORD_AXES 7

int controller_mapping_guid_match(const char *line, const char *guid) {
    if (line == NULL || guid == NULL || strlen(line) <= GUID_LEN || line[GUID_LEN] != ',' || !is_guid(line) ||
        strlen(guid) < GUID_LEN || !is_guid(guid)) {
        return 0;
    }
    bool exact = true, versionless = true;
    for (int i = 0; i < GUID_LEN; i++) {
        if (i / 4 == GUID_WORD_CRC) {
            continue;
        }
        if (tolower((unsigned char) line[i]) != tolower((unsigned char) guid[i])) {
            exact = false;
            if (i / 4 != GUID_WORD_BUTTONS) {
                versionless = false;
            }
        }
    }
    if (exact) {
        return 2;
    }
    return versionless && guid_word(guid, GUID_WORD_VENDOR) != 0 && guid_word(guid, GUID_WORD_PRODUCT) != 0;
}

controller_buttons_t controller_mapping_guid_buttons(const char *guid) {
    return guid != NULL && strlen(guid) >= GUID_LEN && is_guid(guid) ? guid_word(guid, GUID_WORD_BUTTONS) : 0;
}

bool controller_mapping_from_key_codes(char *out, size_t size, const char *guid, int sdk) {
    if (guid == NULL || strlen(guid) < GUID_LEN || !is_guid(guid) || size == 0) {
        return false;
    }
    unsigned buttons = guid_word(guid, GUID_WORD_BUTTONS);
    const unsigned axes = guid_word(guid, GUID_WORD_AXES);
    if ((buttons == 0 && axes == 0) || (buttons & 0xF) == 0) {
        return false;
    }
    int n = snprintf(out, size, "%.*s,*,", GUID_LEN, guid);
#define APPEND(text) do { if (n >= 0 && (size_t) n < size) n += snprintf(out + n, size - (size_t) n, "%s", text); } while (0)
    if (buttons & (1 << 0)) APPEND("a:b0,");
    if (buttons & (1 << 1)) {
        APPEND("b:b1,");
    } else if (buttons & (1 << 4)) {
        /* SDL: the back button as "B" for easy UI navigation with TV remotes */
        APPEND("b:b4,");
        buttons &= ~(1u << 4);
    }
    if (buttons & (1 << 2)) APPEND("x:b2,");
    if (buttons & (1 << 3)) APPEND("y:b3,");
    if (buttons & (1 << 4)) APPEND("back:b4,");
    /* SDL: before Android 11 the guide button generally isn't functional (or acts as a home button) */
    if ((buttons & (1 << 5)) && sdk >= 30) APPEND("guide:b5,");
    if (buttons & (1 << 6)) APPEND("start:b6,");
    if (buttons & (1 << 7)) APPEND("leftstick:b7,");
    if (buttons & (1 << 8)) APPEND("rightstick:b8,");
    if (buttons & (1 << 9)) APPEND("leftshoulder:b9,");
    if (buttons & (1 << 10)) APPEND("rightshoulder:b10,");
    if (buttons & (1 << 11)) APPEND("dpup:b11,");
    if (buttons & (1 << 12)) APPEND("dpdown:b12,");
    if (buttons & (1 << 13)) APPEND("dpleft:b13,");
    if (buttons & (1 << 14)) APPEND("dpright:b14,");
    if (axes & (1 << 0)) APPEND("leftx:a0,");
    if (axes & (1 << 1)) APPEND("lefty:a1,");
    if (axes & (1 << 2)) APPEND("rightx:a2,");
    if (axes & (1 << 3)) APPEND("righty:a3,");
    if (axes & (1 << 4)) APPEND("lefttrigger:a4,");
    if (axes & (1 << 5)) APPEND("righttrigger:a5,");
#undef APPEND
    return n >= 0 && (size_t) n < size;
}

bool controller_mapping_conditions_hold(const char *mapping, int sdk) {
    if (mapping == NULL) {
        return false;
    }
    const char *tmp = strstr(mapping, "hint:");
    if (tmp != NULL) {
        tmp += 5;
        const bool negate = *tmp == '!';
        if (negate) {
            tmp++;
        }
        char hint[128];
        size_t len = 0;
        while (*tmp && *tmp != ',' && *tmp != ':' && len < sizeof(hint) - 1) {
            hint[len++] = *tmp++;
        }
        hint[len] = '\0';
        const SDL_bool default_value = tmp[0] == ':' && tmp[1] == '=' ? (SDL_bool) (atoi(tmp + 2) != 0) : SDL_FALSE;
        bool value = SDL_GetHintBoolean(hint, default_value) == SDL_TRUE;
        if (negate) {
            value = !value;
        }
        if (!value) {
            return false;
        }
    }
    tmp = strstr(mapping, "sdk>=:");
    if (tmp != NULL && !(sdk >= atoi(tmp + 6))) {
        return false;
    }
    tmp = strstr(mapping, "sdk<=:");
    if (tmp != NULL && !(sdk <= atoi(tmp + 6))) {
        return false;
    }
    return true;
}
