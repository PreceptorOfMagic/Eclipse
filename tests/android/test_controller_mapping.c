/*
 * Choosing a controller's mapping by the buttons it can produce (controller_mapping_android.h), on the real lines:
 * SDL 2.32.10's own table (src/joystick/SDL_gamecontrollerdb.h) and the gamecontrollerdb.txt the SHIELD TV
 * downloaded (2026-10-22).
 */
#include "controller_mapping_android.h"

#include <SDL_hints.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

#define ALL (~(controller_buttons_t) 0)
#define B(n) ((controller_buttons_t) 1 << (n))
#define RANGE(lo, hi) ((B((hi) + 1) - 1) & ~(B(lo) - 1))

/* Nintendo Switch Pro Controller: SDL's Android 10+ and Android 9 lines (button labels on), the database's. */
static const char *SWITCH_SDL_10 =
        "050000007e05000009200000ffff0f00,Nintendo Switch Pro Controller,a:b1,b:b0,back:b4,dpdown:b12,dpleft:b13,"
        "dpright:b14,dpup:b11,guide:b5,leftshoulder:b9,leftstick:b7,lefttrigger:b15,leftx:a0,lefty:a1,"
        "rightshoulder:b10,rightstick:b8,righttrigger:b16,rightx:a2,righty:a3,start:b6,x:b3,y:b2,sdk>=:29,"
        "hint:SDL_GAMECONTROLLER_USE_BUTTON_LABELS:=1,";
static const char *SWITCH_SDL_9 =
        "050000007e05000009200000ffff0f00,Nintendo Switch Pro Controller,a:b0,b:b1,back:b15,dpdown:b12,dpleft:b13,"
        "dpright:b14,dpup:b11,guide:b5,leftshoulder:b3,leftstick:b4,lefttrigger:b9,leftx:a0,lefty:a1,"
        "rightshoulder:b18,rightstick:b6,righttrigger:b10,rightx:a2,righty:a3,start:b16,x:b17,y:b2,sdk<=:28,"
        "hint:SDL_GAMECONTROLLER_USE_BUTTON_LABELS:=1,";
static const char *SWITCH_DB =
        "050000007e05000009200000ffff0f00,Nintendo Switch Pro Controller,a:b0,b:b1,back:b15,dpdown:b12,dpleft:b13,"
        "dpright:b14,dpup:b11,guide:b5,leftshoulder:b3,leftstick:b4,lefttrigger:b9,leftx:a0,lefty:a1,"
        "rightshoulder:b18,rightstick:b6,righttrigger:b10,rightx:a2,righty:a3,start:b16,x:b17,y:b2,"
        "platform:Android,";
/* As the SHIELD's Switch Pro reports itself (its CRC cleared). */
static const char *SWITCH_GUID = "050000007e05000009200000ffff0f00";

/* Xbox Series controller: SDL's line (button mask ffff) and the database's, which adds Share as b15. The SHIELD's
 * controller reports 7fff: no b15. */
static const char *XBOX_SDL =
        "050000005e040000130b0000ffff3f00,Xbox Series X Controller,a:b0,b:b1,back:b4,dpdown:b12,dpleft:b13,"
        "dpright:b14,dpup:b11,guide:b5,leftshoulder:b9,leftstick:b7,lefttrigger:a4,leftx:a0,lefty:a1,"
        "rightshoulder:b10,rightstick:b8,righttrigger:a5,rightx:a2,righty:a3,start:b6,x:b2,y:b3,";
static const char *XBOX_DB =
        "050000005e040000130b0000ffff3f00,Xbox Series Controller,a:b0,b:b1,back:b4,dpdown:b12,dpleft:b13,"
        "dpright:b14,dpup:b11,guide:b5,leftshoulder:b9,leftstick:b7,lefttrigger:a4,leftx:a0,lefty:a1,misc1:b15,"
        "rightshoulder:b10,rightstick:b8,righttrigger:a5,rightx:a2,righty:a3,start:b6,x:b2,y:b3,platform:Android,";
static const char *XBOX_GUID = "050000005e040000130b0000ff7f3f00";

static void test_switch_pro(void) {
    char generated[1024];
    CHECK(controller_mapping_from_key_codes(generated, sizeof(generated), SWITCH_GUID, 30));

    /* Android 11 (SHIELD TV): every standard key, L2/R2 as ZL/ZR, no C or Z. SDL's own line fits; the database's
     * binds X and R to keys that never come (C, Z); the key codes alone leave ZL and ZR unbound. */
    const controller_buttons_t android11 = RANGE(0, 16);
    controller_mapping_candidate_t c11[] = {
            {SWITCH_SDL_10, CONTROLLER_MAPPING_SDL_BY_VERSION},
            {SWITCH_DB, CONTROLLER_MAPPING_DATABASE},
            {generated, CONTROLLER_MAPPING_KEY_CODES},
    };
    controller_mapping_fit_t fits[3];
    CHECK(controller_mapping_choose(c11, 3, android11, ALL, fits) == 0);
    CHECK(fits[0].dead == 0 && fits[0].unused == 0);
    CHECK(fits[1].dead == 2);
    CHECK(fits[2].dead == 0 && fits[2].unused == 2);
    /* With no SDL line at all, the key codes beat the database: a wrong layout is worse than unbound triggers. */
    CHECK(controller_mapping_choose(c11 + 1, 2, android11, ALL, NULL) == 1);

    /* Android 9's HID-order layout: C and Z exist. The Android 9 lines fit, the key codes leave four keys unused;
     * SDL's own wins the tie with the database's. */
    const controller_buttons_t android9 = RANGE(0, 18);
    char generated9[1024];
    CHECK(controller_mapping_from_key_codes(generated9, sizeof(generated9), SWITCH_GUID, 28));
    controller_mapping_candidate_t c9[] = {
            {SWITCH_DB, CONTROLLER_MAPPING_DATABASE},
            {SWITCH_SDL_9, CONTROLLER_MAPPING_SDL_BY_VERSION},
            {generated9, CONTROLLER_MAPPING_KEY_CODES},
    };
    CHECK(controller_mapping_choose(c9, 3, android9, ALL, fits) == 1);
    CHECK(fits[0].dead == 0 && fits[1].dead == 0 && fits[0].unused == fits[1].unused);
    CHECK(fits[2].unused > fits[1].unused);

    /* Knowing only the GUID's 16 buttons, keys above b15 are not judged: no line is called dead for them, and the
     * order of the sources decides. */
    controller_mapping_choose(c11, 3, android11, 0xFFFF, fits);
    CHECK(fits[1].dead == 0);
    CHECK(controller_mapping_choose(c11, 3, android11, 0xFFFF, NULL) == 0);
}

static void test_xbox_series(void) {
    /* The database's Share binding is to b15, which the SHIELD's controller never produces: SDL's own fits. */
    char generated[1024];
    CHECK(controller_mapping_from_key_codes(generated, sizeof(generated), XBOX_GUID, 30));
    controller_mapping_candidate_t c[] = {
            {XBOX_SDL, CONTROLLER_MAPPING_SDL},
            {XBOX_DB, CONTROLLER_MAPPING_DATABASE},
            {generated, CONTROLLER_MAPPING_KEY_CODES},
    };
    controller_mapping_fit_t fits[3];
    CHECK(controller_mapping_choose(c, 3, RANGE(0, 14), ALL, fits) == 0);
    CHECK(fits[1].dead == 1);
    /* Where the controller does produce b15, both fit (SDL's takes it for the L2 key beside its analog trigger)
     * and the database's wins the tie: Share works. */
    CHECK(controller_mapping_choose(c, 3, RANGE(0, 15), ALL, fits) == 1);
    CHECK(fits[0].dead == 0 && fits[0].unused == 0 && fits[1].dead == 0 && fits[1].unused == 0);
}

static void test_generic_joystick(void) {
    /* A controller Android has no layout for: BUTTON_1..12 (b20..b31) and a hat. SDL makes nothing from the key
     * codes (no face button); a database line in those terms fits. */
    const char *guid = "05000000aa110000bb22000000780300";
    char generated[1024];
    CHECK(!controller_mapping_from_key_codes(generated, sizeof(generated), guid, 30));
    const char *line = "05000000aa110000bb22000000780300,Generic Pad,a:b21,b:b22,x:b20,y:b23,leftshoulder:b24,"
                       "rightshoulder:b25,lefttrigger:b26,righttrigger:b27,back:b28,start:b29,leftstick:b30,"
                       "rightstick:b31,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,platform:Android,";
    controller_mapping_fit_t fit = controller_mapping_fit(line, RANGE(11, 14) | RANGE(20, 31), ALL);
    CHECK(fit.dead == 0 && fit.unused == 0);
    /* A hat binding can never work on Android (SDL turns hats into the d-pad buttons). */
    fit = controller_mapping_fit("05000000aa110000bb22000000780300,Pad,a:b21,dpup:h0.1,", B(21), ALL);
    CHECK(fit.dead == 1);
}

static void test_fit_details(void) {
    /* L2/R2 keys alongside analog triggers are used while the triggers are bound to axes. */
    const char *axes = "050000004c050000cc090000ffff3f00,Pad,a:b0,b:b1,x:b2,y:b3,lefttrigger:a4,righttrigger:a5,";
    controller_mapping_fit_t fit = controller_mapping_fit(axes, RANGE(0, 3) | B(15) | B(16), ALL);
    CHECK(fit.dead == 0 && fit.unused == 0);
    /* Half-axis targets, inverted sources and the condition fields are not bindings to judge. */
    fit = controller_mapping_fit("050000004c050000cc090000ffff3f00,Pad,+leftx:+a0,-lefty:a1~,a:b0,crc:b0f1,"
                                 "sdk>=:29,hint:!SDL_X:=1,platform:Android,", B(0), ALL);
    CHECK(fit.dead == 0 && fit.unused == 0);
    /* Each binding to a missing button counts. */
    fit = controller_mapping_fit("050000004c050000cc090000ffff3f00,Pad,a:b0,x:b17,y:b17,", B(0), ALL);
    CHECK(fit.dead == 2);
    CHECK(controller_mapping_fit(NULL, B(0), ALL).unused == 1);
}

static void test_key_code_mapping(void) {
    /* SDL_CreateMappingForAndroidController, as SDL 2.32.10 writes it. */
    char out[1024];
    CHECK(controller_mapping_from_key_codes(out, sizeof(out), XBOX_GUID, 30));
    CHECK(strcmp(out, "050000005e040000130b0000ff7f3f00,*,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,"
                      "leftstick:b7,rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,"
                      "dpleft:b13,dpright:b14,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,"
                      "righttrigger:a5,") == 0);
    /* Before Android 11 no guide; a remote with Back and no B gets Back as B. */
    CHECK(controller_mapping_from_key_codes(out, sizeof(out), XBOX_GUID, 28) && strstr(out, "guide") == NULL);
    CHECK(controller_mapping_from_key_codes(out, sizeof(out), "05000000aa110000bb22000011000000", 30));
    CHECK(strcmp(out, "05000000aa110000bb22000011000000,*,a:b0,b:b4,") == 0);
    CHECK(!controller_mapping_from_key_codes(out, 8, XBOX_GUID, 30));
}

static void test_guid_match(void) {
    CHECK(controller_mapping_guid_match(SWITCH_DB, SWITCH_GUID) == 2);
    /* CRCs are ignored, upper case is fine. */
    CHECK(controller_mapping_guid_match(SWITCH_DB, "050069BC7E05000009200000FFFF0F00") == 2);
    /* SDL's fallback: the button mask ignored when a vendor and a product are named... */
    CHECK(controller_mapping_guid_match(XBOX_DB, XBOX_GUID) == 1);
    /* ...but nothing else, and never without both. */
    CHECK(controller_mapping_guid_match(XBOX_DB, "050000005e040000130b0000ff7f3f01") == 0);
    CHECK(controller_mapping_guid_match("05000000000000000000000000000000,X,a:b0,", "05000000000000000000000001000000")
          == 0);
    CHECK(controller_mapping_guid_match("31316661666466633938376335383661,Old,a:b0,", SWITCH_GUID) == 0);
    CHECK(controller_mapping_guid_match("bad", SWITCH_GUID) == 0);
    CHECK(controller_mapping_guid_buttons(XBOX_GUID) == 0x7FFF);
    CHECK(controller_mapping_guid_buttons(SWITCH_GUID) == 0xFFFF);
}

static void test_conditions(void) {
    CHECK(controller_mapping_conditions_hold(SWITCH_DB, 30));
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "1");
    CHECK(controller_mapping_conditions_hold(SWITCH_SDL_10, 30));
    CHECK(!controller_mapping_conditions_hold(SWITCH_SDL_10, 28));
    CHECK(controller_mapping_conditions_hold(SWITCH_SDL_9, 28));
    CHECK(!controller_mapping_conditions_hold(SWITCH_SDL_9, 30));
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    CHECK(!controller_mapping_conditions_hold(SWITCH_SDL_10, 30));
}

int main(void) {
    test_switch_pro();
    test_xbox_series();
    test_generic_joystick();
    test_fit_details();
    test_key_code_mapping();
    test_guid_match();
    test_conditions();
    if (failures == 0) {
        printf("test_controller_mapping: OK\n");
    }
    return failures == 0 ? 0 : 1;
}
