/*
 * WHICH CONTROLLER MAPPING FITS, judged per controller against the buttons Android says it can produce
 * (input_gamepad_mapping.c asks when a controller connects).
 *
 * On Android SDL numbers a controller's buttons by the KEY CODE Android delivers (SDL 2.32.10,
 * src/joystick/android/SDL_sysjoystick.c keycode_to_SDL: BUTTON_A is b0, START b6, L2 b15, R2 b16, C b17,
 * Z b18, BUTTON_1..16 b20..b35), and Android picks the key codes from its key layout for that controller, which
 * changes between Android versions: SDL's own table keeps two layouts of the Nintendo Switch Pro Controller,
 * one for Android 10 and later and one, numbered in HID order, for Android 9 and earlier. A mapping records one
 * layout and its GUID cannot say which: the GUID carries only the low 16 bits of the button set, and SDL even
 * matches Android mappings with those ignored. So a mapping written on one Android version scrambles the
 * controller on another -- the downloaded gamecontrollerdb.txt's Switch Pro line is the Android 9 layout, and on
 * the SHIELD TV (Android 11) it made ZR Start and + the right stick, so no shortcut worked (2026-10-22).
 *
 * The controller is the evidence. A binding to a button it cannot produce is DEAD (that function can never be
 * pressed); a button it can produce that nothing is bound to is UNUSED. The fewest dead bindings wins, then the
 * fewest unused buttons, then the source: SDL's own mapping where SDL chose it by Android version, a database
 * line, SDL's own otherwise, and last the mapping SDL builds from the key codes alone.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** SDL's joystick buttons on Android: bit N is bN. */
typedef uint64_t controller_buttons_t;

typedef struct controller_mapping_fit_t {
    /** Bindings to a button the controller cannot produce, or to a hat (SDL's Android joysticks have none). */
    int dead;
    /** Buttons it can produce that nothing is bound to (L2/R2 count as bound while a trigger is bound). */
    int unused;
} controller_mapping_fit_t;

/** Where a candidate came from, in the order a tie is settled. */
typedef enum controller_mapping_source_t {
    CONTROLLER_MAPPING_SDL_BY_VERSION = 0,  /* SDL's own, chosen by Android version (sdk>=: / sdk<=:) */
    CONTROLLER_MAPPING_DATABASE,            /* a gamecontrollerdb.txt line */
    CONTROLLER_MAPPING_SDL,                 /* SDL's own table, for any Android version */
    CONTROLLER_MAPPING_KEY_CODES,           /* built from the key codes alone */
} controller_mapping_source_t;

typedef struct controller_mapping_candidate_t {
    const char *mapping;
    controller_mapping_source_t source;
} controller_mapping_candidate_t;

/** How @p mapping fits a controller producing @p buttons. Only the buttons in @p known are judged: a binding to
 *  a button outside it is neither dead nor counted against the controller. */
controller_mapping_fit_t controller_mapping_fit(const char *mapping, controller_buttons_t buttons,
                                                controller_buttons_t known);

/** The index of the best candidate, -1 if @p count is 0. @p fits (NULL, or @p count entries) gets each fit. */
int controller_mapping_choose(const controller_mapping_candidate_t *candidates, int count,
                              controller_buttons_t buttons, controller_buttons_t known,
                              controller_mapping_fit_t *fits);

/** The mapping SDL builds for an Android controller it has none for (SDL 2.32.10
 *  SDL_CreateMappingForAndroidController), from @p guid's button and axis masks, named "*" as SDL names it.
 *  false where SDL makes none (no face button). */
bool controller_mapping_from_key_codes(char *out, size_t size, const char *guid, int sdk);

/** Whether the mapping @p line is for @p guid (both 32 hex digits; CRCs ignored): 2 exactly, 1 with the
 *  button-mask bytes ignored -- the match SDL falls back to when the GUID names a vendor and a product -- 0 not. */
int controller_mapping_guid_match(const char *line, const char *guid);

/** Whether @p mapping's own conditions hold (sdk>=:, sdk<=:, hint:), tested as SDL tests them when adding it. */
bool controller_mapping_conditions_hold(const char *mapping, int sdk);

/** The buttons @p guid's mask records: b0..b15 only (the d-pad when the controller reports a hat). */
controller_buttons_t controller_mapping_guid_buttons(const char *guid);
