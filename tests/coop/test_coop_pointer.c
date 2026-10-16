/*
 * The co-op pointer split: which PC does the magic remote's cursor address, and where
 * on that PC does it land.
 *
 * WHAT THIS IS FOR. The whole feature is one piece of arithmetic -- window position ->
 * (pane, pane-local position) -- and getting it wrong is not a crash, it is a cursor
 * that lands somewhere plausible but wrong, on a screen only the user can see. That is
 * the failure mode this project keeps re-learning (CLAUDE.md: the logs looked fine).
 * So the arithmetic is asserted here, on a dev box, and the LIVE test on the TV is left
 * to check the things arithmetic cannot: that it feels instant and lines up with what
 * you are aiming at.
 *
 * Standalone like the rest of tests/coop -- no fixtures, no host, no hardware:
 *   gcc -std=gnu11 -Wall -Wextra -Werror -I core/moonlight-common-c/src \
 *       -DCOOP_PTR_VMIRROR_SENTINEL='"/tmp/test-coop-ptr-vmirror"' \
 *       -DAURORA_DIAG_LOG='"/tmp/test-coop-ptr-diag.log"' \
 *       -o /tmp/test_coop_pointer tests/coop/test_coop_pointer.c \
 *       src/app/stream/input/coop_pointer.c && /tmp/test_coop_pointer
 *
 * The sentinel is redirected to a scratch path on purpose: a unit test must not arm a
 * real mode selector as a side effect.
 */
#include "../../src/app/stream/input/coop_pointer.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <Limelight.h>

#define SENTINEL "/tmp/test-coop-ptr-vmirror"

static int fails = 0;
static void ck(const char *what, int ok) {
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

/* ── what the module did, captured ────────────────────────────────────────── */

enum { TO_NONE = 0, TO_PRIMARY, TO_PEER };

static struct {
    int target;
    int x, y, w, h;
} pos;

static struct {
    int target, action, button, count;
} btn;

static struct {
    int target, vertical, horizontal;
} scr;

static void reset_capture(void) {
    memset(&pos, 0, sizeof(pos));
    memset(&btn, 0, sizeof(btn));
    memset(&scr, 0, sizeof(scr));
}

/* The primary's half: the real Limelight entry points, stubbed. */
int LiSendMousePositionEvent(short x, short y, short referenceWidth, short referenceHeight) {
    pos.target = TO_PRIMARY;
    pos.x = x; pos.y = y; pos.w = referenceWidth; pos.h = referenceHeight;
    return 0;
}

int LiSendMouseButtonEvent(char action, int button) {
    btn.target = TO_PRIMARY; btn.action = action; btn.button = button; btn.count++;
    return 0;
}

int LiSendScrollEvent(signed char scrollClicks) {
    scr.target = TO_PRIMARY; scr.vertical = scrollClicks;
    return 0;
}

int LiSendHScrollEvent(signed char scrollClicks) {
    scr.target = TO_PRIMARY; scr.horizontal = scrollClicks;
    return 0;
}

/* coop_pointer_scroll_high_res (the Android virtual mouse) only; not exercised here. */
int LiSendHighResScrollEvent(short scrollAmount) {
    (void) scrollAmount;
    return 0;
}

/* The peer's half: the co-op input link, stubbed. */
void coop_input_send_mouse_position(int x, int y, int ref_w, int ref_h) {
    pos.target = TO_PEER;
    pos.x = x; pos.y = y; pos.w = ref_w; pos.h = ref_h;
}

void coop_input_send_mouse_button(int action, int button) {
    btn.target = TO_PEER; btn.action = action; btn.button = button; btn.count++;
}

void coop_input_send_mouse_scroll(int vertical, int horizontal) {
    scr.target = TO_PEER; scr.vertical = vertical; scr.horizontal = horizontal;
}

/* Is a secondary attached? Default yes, so every existing case keeps testing the split
 * itself. The "no peer" case below is the regression test for input that vanished. */
static bool stub_peer_attached = true;

bool coop_input_peer_attached(void) { return stub_peer_attached; }

/* ── helpers ──────────────────────────────────────────────────────────────── */

/* The TV window the magic remote reports in. */
#define W 1920
#define H 1080

static void arm(int vertical) {
    unlink(SENTINEL);
    if (vertical) {
        FILE *f = fopen(SENTINEL, "w");
        if (f != NULL) {
            fclose(f);
        }
    }
    coop_pointer_start();
}

static int at(int target, int x, int y, int w, int h) {
    return pos.target == target && pos.x == x && pos.y == y && pos.w == w && pos.h == h;
}

static void say(const char *label) {
    printf("     %s -> %s (%d,%d) of %dx%d\n", label,
           pos.target == TO_PEER ? "PEER" : pos.target == TO_PRIMARY ? "PRIMARY" : "nobody",
           pos.x, pos.y, pos.w, pos.h);
}

int main(void) {
    printf("1. no co-op: the single-host path is untouched\n");
    {
        coop_pointer_stop();
        reset_capture();
        coop_pointer_position(960, 540, W, H);
        say("centre of the screen");
        ck("goes to the primary, unmapped, against the whole window",
           at(TO_PRIMARY, 960, 540, W, H));
        coop_pointer_scroll(3, 0);
        ck("scroll goes to the primary", scr.target == TO_PRIMARY && scr.vertical == 3);
        ck("not active", coop_pointer_active() == false);
    }

    printf("2. HORIZONTAL: primary is the TOP pane, peer the BOTTOM\n");
    {
        arm(0);
        ck("active", coop_pointer_active() == true);

        coop_pointer_position(960, 100, W, H);
        say("upper half");
        ck("primary, and the pane is half the height", at(TO_PRIMARY, 960, 100, W, 540));

        coop_pointer_position(960, 539, W, H);
        say("one pixel above the divider");
        ck("still the primary, at the BOTTOM of its screen", at(TO_PRIMARY, 960, 539, W, 540));

        coop_pointer_position(960, 540, W, H);
        say("one pixel below the divider");
        ck("switched to the peer, at the TOP of its screen, same x",
           at(TO_PEER, 960, 0, W, 540));

        coop_pointer_position(960, 1079, W, H);
        say("bottom of the screen");
        ck("peer, at the bottom of its screen", at(TO_PEER, 960, 539, W, 540));

        /* The axis ALONG the divider must survive the crossing untouched, or the
         * cursor would jump sideways as it changed machine. */
        coop_pointer_position(300, 1000, W, H);
        ck("x is carried across unchanged", pos.x == 300);

        coop_pointer_position(300, 200, W, H);
        say("back above the divider");
        ck("crossed back to the primary", at(TO_PRIMARY, 300, 200, W, 540));
        ck("two crossings counted", coop_pointer_crossings() == 2);
    }

    printf("3. VERTICAL: primary is the LEFT pane, peer the RIGHT\n");
    {
        arm(1);

        coop_pointer_position(100, 500, W, H);
        say("left half");
        ck("primary, and the pane is half the width", at(TO_PRIMARY, 100, 500, 960, H));

        coop_pointer_position(959, 500, W, H);
        say("one pixel left of the divider");
        ck("still the primary, at the RIGHT edge of its screen", at(TO_PRIMARY, 959, 500, 960, H));

        coop_pointer_position(960, 500, W, H);
        say("one pixel right of the divider");
        ck("switched to the peer, at the LEFT edge of its screen, same y",
           at(TO_PEER, 0, 500, 960, H));

        coop_pointer_position(1919, 20, W, H);
        say("top right corner");
        ck("peer, top right of its screen", at(TO_PEER, 959, 20, 960, H));
        ck("y is carried across unchanged", pos.y == 20);
    }

    printf("4. buttons and wheel follow the pane the pointer is over\n");
    {
        arm(0);
        reset_capture();

        coop_pointer_position(500, 900, W, H);          /* peer's pane */
        coop_pointer_button(BUTTON_ACTION_PRESS, BUTTON_LEFT);
        ck("press went to the peer", btn.target == TO_PEER && btn.button == BUTTON_LEFT);
        coop_pointer_button(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
        ck("release went to the peer", btn.target == TO_PEER);

        coop_pointer_scroll(-2, 0);
        ck("wheel went to the peer", scr.target == TO_PEER && scr.vertical == -2);

        coop_pointer_position(500, 100, W, H);          /* primary's pane */
        coop_pointer_scroll(0, 1);
        ck("wheel follows back to the primary",
           scr.target == TO_PRIMARY && scr.horizontal == 1);
    }

    printf("5. a held button pins the pane: no drag hand-off, no stuck button\n");
    {
        arm(0);
        reset_capture();

        coop_pointer_position(500, 300, W, H);
        coop_pointer_button(BUTTON_ACTION_PRESS, BUTTON_LEFT);
        ck("press on the primary", btn.target == TO_PRIMARY);

        coop_pointer_position(500, 900, W, H);          /* dragged past the divider */
        say("dragged into the peer's pane");
        ck("stayed on the primary", pos.target == TO_PRIMARY);
        ck("clamped to the primary's bottom edge", pos.y == 539);
        ck("no crossing counted while pinned", coop_pointer_crossings() == 0);

        coop_pointer_button(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
        ck("release reached the pane that took the press", btn.target == TO_PRIMARY);

        coop_pointer_position(500, 900, W, H);          /* free again */
        say("same position, button now up");
        ck("now switches to the peer", at(TO_PEER, 500, 360, W, 540));
        ck("one crossing counted", coop_pointer_crossings() == 1);
    }

    printf("6. stopping puts the single-host path back\n");
    {
        coop_pointer_stop();
        reset_capture();
        coop_pointer_position(960, 900, W, H);
        ck("unmapped again", at(TO_PRIMARY, 960, 900, W, H));
    }

    printf("7. SPLIT ARMED BUT NO PEER ATTACHED: input must still land somewhere\n");
    {
        /* The 2026-08-12 regression. Co-op mode leaked, so a single-host stream came up
         * with the split armed; everything in the bottom half was addressed to a
         * secondary that did not exist and was dropped on the floor. The user had no
         * control below the midline and nothing said why. Input must never vanish. */
        arm(0);
        stub_peer_attached = false;
        reset_capture();

        coop_pointer_position(960, 900, W, H);
        say("bottom half with no secondary attached");
        ck("falls back to the primary, unmapped", at(TO_PRIMARY, 960, 900, W, H));

        coop_pointer_button(BUTTON_ACTION_PRESS, BUTTON_LEFT);
        ck("the CLICK reaches the primary rather than a dead socket",
           btn.target == TO_PRIMARY && btn.count == 1);

        reset_capture();
        coop_pointer_scroll(3, 0);
        ck("scroll reaches the primary too",
           scr.target == TO_PRIMARY && scr.vertical == 3);

        /* And once a peer really is there, the split behaves exactly as before. */
        stub_peer_attached = true;
        reset_capture();
        coop_pointer_position(960, 900, W, H);
        ck("with a peer attached it routes to the peer again",
           at(TO_PEER, 960, 360, W, 540));
        stub_peer_attached = true;
    }

    printf("8. THIS PC IS ONE OF THE HOSTS: nothing is ever sent to its pane\n");
    {
        /* Input for a host that is this very machine loops back through its desktop (Windows
         * client, 2026-09-26); its pane is handed the real cursor instead, which off Windows is
         * a no-op. Either way no position may be SENT there -- and the other pane is untouched. */
        coop_pointer_stop();
        coop_pointer_set_local_pane(1);   /* the peer's host is this PC */
        arm(1);                           /* vertical: primary LEFT, peer RIGHT */
        reset_capture();
        coop_pointer_position(1500, 500, W, H);
        say("right half = this PC's pane");
        ck("nothing sent to this PC's pane", pos.target == TO_NONE);

        coop_pointer_position(200, 500, W, H);
        say("left half = the other PC");
        ck("the other PC's pane still gets the pointer", at(TO_PRIMARY, 200, 500, 960, H));

        coop_pointer_stop();
        coop_pointer_set_local_pane(-1);
        arm(1);
        reset_capture();
        coop_pointer_position(1500, 500, W, H);
        ck("cleared: the peer's pane is sent to again", at(TO_PEER, 540, 500, 960, H));
        coop_pointer_stop();
    }

    printf("9. MOUSE AND KEYBOARD ASSIGNED LIKE CONTROLLERS: no crossing, keys stay put\n");
    {
        /* Issue #37: an assigned mouse belongs to one PC for the session -- the pointer is
         * clamped into that pane instead of crossing -- and assigned keys go to their pane
         * wherever the pointer is. */
        coop_pointer_stop();
        coop_pointer_set_pins(1, 1);      /* mouse and keyboard: the peer (RIGHT pane) */
        arm(1);
        reset_capture();
        coop_pointer_position(200, 500, W, H);
        say("left half, mouse assigned to the peer");
        ck("stays on the peer, clamped to its left edge", at(TO_PEER, 0, 500, 960, H));
        ck("no crossing counted", coop_pointer_crossings() == 1);   /* the one move into the pinned pane */
        ck("keys go to the peer", coop_pointer_keys_to_peer() == true);

        coop_pointer_stop();
        coop_pointer_set_pins(-1, 0);     /* mouse follows, keyboard assigned to the primary */
        arm(1);
        reset_capture();
        coop_pointer_position(1500, 500, W, H);
        ck("the mouse still crosses to the peer", at(TO_PEER, 540, 500, 960, H));
        ck("but the keys stay with the primary", coop_pointer_keys_to_peer() == false);
        coop_pointer_stop();
        coop_pointer_set_pins(-1, -1);
    }

    unlink(SENTINEL);
    printf(fails == 0 ? "\nALL PASS\n" : "\n%d FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
