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

static void dpad(int flag) {
    pad.buttons |= flag;
    input();
    pad.buttons &= ~flag;
    input();
}

/* The d-pad moves the highlight a step per press and letting it go chooses nothing (user, 2026-10-22: it was
 * "exiting instantly after clicking any direction"); the opposite direction goes back to the centre. */
static void testDpadSteps(void) {
    hold_start();
    pad.buttons |= RIGHT_FLAG;
    start_wheel_update_t update = input();
    TEST_ASSERT_TRUE(update.neutral);
    TEST_ASSERT_TRUE(update.changed);
    TEST_ASSERT_EQUAL(START_WHEEL_MOUSE, wheel.selected);
    pad.buttons &= ~RIGHT_FLAG;
    update = input();
    TEST_ASSERT_FALSE(update.hide);
    TEST_ASSERT_TRUE(update.consume);
    TEST_ASSERT_EQUAL(START_WHEEL_VISIBLE, wheel.state);
    TEST_ASSERT_EQUAL(START_WHEEL_MOUSE, wheel.selected);
    dpad(LEFT_FLAG);
    TEST_ASSERT_EQUAL(START_WHEEL_CONTINUE, wheel.selected);
    dpad(LEFT_FLAG);
    TEST_ASSERT_EQUAL(START_WHEEL_PERFORMANCE, wheel.selected);
    dpad(UP_FLAG);                          /* not the opposite: straight to that option */
    TEST_ASSERT_EQUAL(START_WHEEL_MENU, wheel.selected);
    pad.buttons = 0;                        /* Start let go: chooses */
    update = input();
    TEST_ASSERT_TRUE(update.hide);
    TEST_ASSERT_EQUAL(START_WHEEL_MENU, update.run);
}

/* B takes the highlight back to Continue -- a way out of a wheel opened by accident -- and is not the game's. */
static void testBackGoesToContinue(void) {
    hold_start();
    pad.rx = FULL;
    input();
    input();
    TEST_ASSERT_EQUAL(START_WHEEL_MOUSE, wheel.selected);
    pad.rx = 0;
    input();
    pad.buttons |= B_FLAG;
    start_wheel_update_t update = input();
    TEST_ASSERT_TRUE(update.consume);
    TEST_ASSERT_EQUAL(START_WHEEL_CONTINUE, wheel.selected);
    pad.buttons = 0;
    update = input();
    TEST_ASSERT_TRUE(update.consume);
    TEST_ASSERT_EQUAL(START_WHEEL_CONTINUE, update.run);
    TEST_ASSERT_EQUAL(START_WHEEL_IDLE, wheel.state);
    /* B on a wheel nothing was pushed on yet: also the wheel's, not the game's. */
    setUp();
    hold_start();
    pad.buttons |= B_FLAG;
    update = input();
    TEST_ASSERT_TRUE(update.neutral);
    TEST_ASSERT_TRUE(update.consume);
}

/* What is held before the wheel shows is not a press on it. */
static void testHeldBeforeShowIsNotAPress(void) {
    pad.buttons = PLAY_FLAG;
    start_wheel_start_down(&wheel, 1000);
    pad.buttons |= RIGHT_FLAG;
    input();                                /* the game's d-pad, while Start is only held */
    start_wheel_hold_elapsed(&wheel, 1000 + START_WHEEL_HOLD_MS);
    const start_wheel_update_t update = input();
    TEST_ASSERT_FALSE(update.consume);
    TEST_ASSERT_EQUAL(START_WHEEL_CONTINUE, wheel.selected);
}

/* Pushed, then back to the centre before letting go: the choice stays (user, 2026-10-22), and runs on release. */
static void testCentreKeepsChoice(void) {
    hold_start();
    pad.rx = FULL;
    input();
    input();
    TEST_ASSERT_EQUAL(START_WHEEL_MOUSE, wheel.selected);
    pad.rx = 0;
    input();
    input();
    input();
    TEST_ASSERT_EQUAL(START_WHEEL_MOUSE, wheel.selected);
    pad.buttons = 0;
    const start_wheel_update_t update = input();
    TEST_ASSERT_TRUE(update.consume);
    TEST_ASSERT_EQUAL(START_WHEEL_MOUSE, update.run);
    TEST_ASSERT_EQUAL(START_WHEEL_IDLE, wheel.state);
}

/* ... and moves on only when pushed towards another option. */
static void testOtherDirectionChanges(void) {
    hold_start();
    pad.rx = FULL;
    input();
    input();
    pad.rx = 0;
    input();
    pad.ly = -FULL;                         /* left stick down */
    input();
    input();
    TEST_ASSERT_EQUAL(START_WHEEL_KEYBOARD, wheel.selected);
}

/* A stick steps like the d-pad (user, 2026-10-22: "it skips over [the centre] on a push of the stick all the way to
 * the other side"): pushed the other way from an option it goes back to Continue, pushed again to the far option. */
static void testStickOppositeGoesToCentre(void) {
    hold_start();
    pad.rx = FULL;
    input();
    input();
    TEST_ASSERT_EQUAL(START_WHEEL_MOUSE, wheel.selected);
    pad.rx = 0;
    input();
    pad.rx = -FULL;
    input();
    input();
    TEST_ASSERT_EQUAL(START_WHEEL_CONTINUE, wheel.selected);
    pad.rx = 0;
    input();
    pad.rx = -FULL;
    input();
    input();
    TEST_ASSERT_EQUAL(START_WHEEL_PERFORMANCE, wheel.selected);
    /* A flick straight across, never seen in the middle, is one step too. */
    pad.rx = FULL;
    input();
    input();
    TEST_ASSERT_EQUAL(START_WHEEL_CONTINUE, wheel.selected);
    /* Held where it points, nothing more happens. */
    input();
    input();
    TEST_ASSERT_EQUAL(START_WHEEL_CONTINUE, wheel.selected);
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
    RUN_TEST(testDpadSteps);
    RUN_TEST(testBackGoesToContinue);
    RUN_TEST(testHeldBeforeShowIsNotAPress);
    RUN_TEST(testCentreKeepsChoice);
    RUN_TEST(testOtherDirectionChanges);
    RUN_TEST(testStickOppositeGoesToCentre);
    RUN_TEST(testDirectionHolds);
    RUN_TEST(testCancel);
    RUN_TEST(testHeldDoesNotCapture);
    return UNITY_END();
}
