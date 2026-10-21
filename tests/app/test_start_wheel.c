/*
 * The Start wheel's gesture (src/app/stream/input/start_wheel.c): what reaches the host, what is chosen and when it runs.
 * The rules are Moonlight V+'s StartGestureReducer (qiin2333/moonlight-vplus 4726655) except that a choice runs once
 * the controller is let go; each test names the rule it holds to.
 */
#include <Limelight.h>

#include "unity.h"
#include "stream/input/start_wheel.h"

#define FULL 32767

static start_wheel_t wheel;
static start_wheel_pad_t pad;

void setUp(void) {
    start_wheel_reset(&wheel);
    pad = (start_wheel_pad_t) {0};
}

void tearDown(void) {
}

static start_wheel_update_t input(void) {
    return start_wheel_input(&wheel, &pad);
}

/** Start down at 0 and held long enough: the wheel shows. */
static void hold_start(void) {
    pad.buttons |= PLAY_FLAG;
    start_wheel_start_down(&wheel, 1000);
    const start_wheel_update_t update = start_wheel_hold_elapsed(&wheel, 1000 + START_WHEEL_HOLD_MS);
    TEST_ASSERT_TRUE(update.show);
    TEST_ASSERT_EQUAL(START_WHEEL_VISIBLE, wheel.state);
}

/* A tap is the host's Start: nothing shows, nothing is held back. */
static void testTapIsStart(void) {
    pad.buttons = PLAY_FLAG;
    start_wheel_start_down(&wheel, 1000);
    TEST_ASSERT_FALSE(start_wheel_hold_elapsed(&wheel, 1000 + START_WHEEL_HOLD_MS / 2).show);
    pad.buttons = 0;
    const start_wheel_update_t update = input();
    TEST_ASSERT_FALSE(update.consume);
    TEST_ASSERT_FALSE(update.hide);
    TEST_ASSERT_EQUAL(START_WHEEL_IDLE, wheel.state);
}

/* Holding without pushing anything only looks: the wheel closes on release and Start's release is the host's. */
static void testLookingOnly(void) {
    hold_start();
    pad.buttons |= A_FLAG;                  /* other buttons still play while nothing is pushed */
    TEST_ASSERT_FALSE(input().consume);
    pad.buttons = 0;
    const start_wheel_update_t update = input();
    TEST_ASSERT_TRUE(update.hide);
    TEST_ASSERT_FALSE(update.consume);
    TEST_ASSERT_EQUAL(START_WHEEL_CONTINUE, update.run);
    TEST_ASSERT_EQUAL(START_WHEEL_IDLE, wheel.state);
}

/* A stick up chooses Menu after two events; the first push takes the controller (one neutral); Start's release
 * chooses, and the menu opens once the stick is back. */
static void testStickChoosesOnStartRelease(void) {
    hold_start();
    pad.ry = FULL;                          /* host sticks are Y up */
    start_wheel_update_t update = input();
    TEST_ASSERT_TRUE(update.neutral);
    TEST_ASSERT_TRUE(update.consume);
    TEST_ASSERT_FALSE(update.changed);      /* one event is not yet a direction */
    update = input();
    TEST_ASSERT_FALSE(update.neutral);      /* once only */
    TEST_ASSERT_TRUE(update.changed);
    TEST_ASSERT_EQUAL(START_WHEEL_MENU, wheel.selected);
    pad.buttons = 0;                        /* Start let go, stick still up */
    update = input();
    TEST_ASSERT_TRUE(update.hide);
    TEST_ASSERT_TRUE(update.consume);
    TEST_ASSERT_EQUAL(START_WHEEL_CONTINUE, update.run);
    TEST_ASSERT_EQUAL(START_WHEEL_WAIT_RELEASE, wheel.state);
    pad.ry = 0;
    update = input();
    TEST_ASSERT_TRUE(update.consume);
    TEST_ASSERT_EQUAL(START_WHEEL_MENU, update.run);
    TEST_ASSERT_EQUAL(START_WHEEL_IDLE, wheel.state);
    TEST_ASSERT_FALSE(input().consume);     /* the controller is the host's again */
}

/* The four directions, from either stick. */
static void testDirections(void) {
    const struct {
        short lx, ly, rx, ry;
        start_wheel_action_t want;
    } cases[] = {
            {0, 0, FULL, 0, START_WHEEL_MOUSE},
            {0, 0, -FULL, 0, START_WHEEL_PERFORMANCE},
            {0, -FULL, 0, 0, START_WHEEL_KEYBOARD},
            {0, FULL, 0, 0, START_WHEEL_MENU},
            {FULL, 0, 0, 0, START_WHEEL_MOUSE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        setUp();
        hold_start();
        pad.lx = cases[i].lx, pad.ly = cases[i].ly, pad.rx = cases[i].rx, pad.ry = cases[i].ry;
        input();
        input();
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].want, wheel.selected, start_wheel_action_name(cases[i].want));
    }
}

/* The d-pad chooses at once and when IT is let go, Start still held; the action waits for Start. */
static void testDpadChoosesOnDpadRelease(void) {
    hold_start();
    pad.buttons |= RIGHT_FLAG;
    start_wheel_update_t update = input();
    TEST_ASSERT_TRUE(update.neutral);
    TEST_ASSERT_TRUE(update.changed);
    TEST_ASSERT_EQUAL(START_WHEEL_MOUSE, wheel.selected);
    pad.buttons &= ~RIGHT_FLAG;
    update = input();
    TEST_ASSERT_TRUE(update.hide);
    TEST_ASSERT_TRUE(update.consume);
    TEST_ASSERT_EQUAL(START_WHEEL_WAIT_RELEASE, wheel.state);
    pad.buttons = 0;
    update = input();
    TEST_ASSERT_TRUE(update.consume);
    TEST_ASSERT_EQUAL(START_WHEEL_MOUSE, update.run);
}

/* Pushed, then back to the centre before letting go: Continue, nothing runs, and the release stays local. */
static void testBackToCentreIsContinue(void) {
    hold_start();
    pad.rx = FULL;
    input();
    input();
    TEST_ASSERT_EQUAL(START_WHEEL_MOUSE, wheel.selected);
    pad.rx = 0;
    input();
    input();
    TEST_ASSERT_EQUAL(START_WHEEL_CONTINUE, wheel.selected);
    pad.buttons = 0;
    const start_wheel_update_t update = input();
    TEST_ASSERT_TRUE(update.consume);
    TEST_ASSERT_EQUAL(START_WHEEL_CONTINUE, update.run);
    TEST_ASSERT_EQUAL(START_WHEEL_IDLE, wheel.state);
}

/* Hysteresis: a chosen direction holds until the stick drops below the exit threshold. */
static void testDirectionHolds(void) {
    hold_start();
    pad.rx = FULL;
    input();
    input();
    pad.rx = FULL / 2;                      /* 0.5: under the 0.65 needed to choose, over the 0.35 to keep */
    input();
    input();
    TEST_ASSERT_EQUAL(START_WHEEL_MOUSE, wheel.selected);
}

/* A cancel (a combo formed, the menu opened another way) hides the wheel and runs nothing. */
static void testCancel(void) {
    hold_start();
    const start_wheel_update_t update = start_wheel_cancel(&wheel);
    TEST_ASSERT_TRUE(update.hide);
    TEST_ASSERT_EQUAL(START_WHEEL_CONTINUE, update.run);
    TEST_ASSERT_EQUAL(START_WHEEL_IDLE, wheel.state);
    TEST_ASSERT_FALSE(start_wheel_cancel(&wheel).hide);
}

/* Before the wheel shows, nothing is the wheel's: the d-pad plays. */
static void testHeldDoesNotCapture(void) {
    pad.buttons = PLAY_FLAG;
    start_wheel_start_down(&wheel, 1000);
    pad.buttons |= UP_FLAG;
    const start_wheel_update_t update = input();
    TEST_ASSERT_FALSE(update.consume);
    TEST_ASSERT_FALSE(update.neutral);
    TEST_ASSERT_EQUAL(START_WHEEL_HELD, wheel.state);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(testTapIsStart);
    RUN_TEST(testLookingOnly);
    RUN_TEST(testStickChoosesOnStartRelease);
    RUN_TEST(testDirections);
    RUN_TEST(testDpadChoosesOnDpadRelease);
    RUN_TEST(testBackToCentreIsContinue);
    RUN_TEST(testDirectionHolds);
    RUN_TEST(testCancel);
    RUN_TEST(testHeldDoesNotCapture);
    return UNITY_END();
}
