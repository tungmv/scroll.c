/*
 * Unit test for the wheel-vs-trackpad decision.
 *
 * The interceptor's filter lives in main.c, which has a main() and links
 * CoreGraphics. To test the pure predicate without that baggage, this file
 * pulls in main.c with main() renamed and checks is_mouse_wheel() directly.
 *
 * The expected values come from real hardware observed on this machine:
 *   - trackpad move/momentum events arrive with IsContinuous clear, which is
 *     exactly why keying on that flag inverted the trackpad
 *   - trackpad events carry a scroll phase (2 = changed, 4 = ended) or a
 *     momentum phase (1..4)
 *   - a real wheel tick carries neither
 */
#define main scroll_program_main
#include "main.c"
#undef main

#include <assert.h>
#include <stdio.h>

static int failures = 0;

static void check(const char *what, int got, int want) {
    if (got != want) {
        printf("FAIL %-58s got=%d want=%d\n", what, got, want);
        failures++;
    } else {
        printf("ok   %-58s\n", what);
    }
}

int main(void) {
    /* Real mouse wheel: no phases, not continuous. */
    check("mouse wheel (cont=0, phase=0, momentum=0)", is_mouse_wheel(0, 0, 0), 1);

    /* Continuous flag set: trackpad / smooth scrolling. */
    check("continuous event (cont=1)", is_mouse_wheel(1, 0, 0), 0);

    /* Trackpad gesture phases, as observed. */
    check("trackpad begin   (cont=0, phase=1)", is_mouse_wheel(0, 1, 0), 0);
    check("trackpad changed (cont=0, phase=2)", is_mouse_wheel(0, 2, 0), 0);
    check("trackpad ended   (cont=0, phase=4)", is_mouse_wheel(0, 4, 0), 0);

    /* Trackpad momentum, the case that used to be inverted with cont == 0. */
    check("trackpad momentum none    (mom=1)", is_mouse_wheel(0, 0, 1), 0);
    check("trackpad momentum begin   (mom=2)", is_mouse_wheel(0, 0, 2), 0);
    check("trackpad momentum continue(mom=3)", is_mouse_wheel(0, 0, 3), 0);
    check("trackpad momentum end     (mom=4)", is_mouse_wheel(0, 0, 4), 0);

    /* Both phases present. */
    check("trackpad phase + momentum", is_mouse_wheel(0, 2, 3), 0);
    check("trackpad continuous + phase", is_mouse_wheel(1, 2, 0), 0);

    /* Sign must not matter (fields are unsigned-ish phases, not deltas). */
    check("mouse wheel, negative-ish flags", is_mouse_wheel(0, 0, 0), 1);

    if (failures) {
        printf("\n%d FAILED\n", failures);
        return 1;
    }
    printf("\nall passed\n");
    return 0;
}
