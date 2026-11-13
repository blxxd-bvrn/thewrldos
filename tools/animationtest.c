/* Host-side checks for the transitions used by the WM, sidebar and Start menu.
 * Run with make check-animations; no framebuffer, scheduler or hardware needed. */
#include "animation.h"
#include <stdio.h>

static int fails;
static void check(const char *what, int got, int want) {
    if (got != want) { printf("FAIL %s: got %d, wanted %d\n", what, got, want); fails++; }
}
static void monotonic(int from, int to, uint32_t start, uint32_t duration) {
    animation_t a;
    animation_set(&a, from);
    animation_start(&a, to, start, duration);
    int last = from;
    for (uint32_t t = 0; t <= duration + 50; t++) {
        int value = animation_value(&a, start + t);
        check("value stays in range", value >= MIN(from, to) && value <= MAX(from, to), 1);
        check("no overshoot or backward steps", from < to ? value >= last : value <= last, 1);
        last = value;
    }
    check("exact final value", last, to);
    check("finished transition is idle", animation_running(&a, start + duration), 0);
}

int main(void) {
    animation_t a = { 0 };
    check("uninitialised transition is idle", animation_running(&a, 100), 0);
    check("uninitialised value", animation_value(&a, 100), 0);
    animation_start(&a, 255, 100, 200);
    check("starts at the existing value", animation_value(&a, 100), 0);
    check("running", animation_running(&a, 101), 1);
    check("midpoint", animation_value(&a, 200), 127);
    check("slow frame skips to the endpoint", animation_value(&a, 1000), 255);
    int current = animation_value(&a, 180);
    animation_start(&a, 0, 180, 200);
    check("reversal does not jump", animation_value(&a, 180), current);
    check("reversal reaches closed", animation_value(&a, 380), 0);
    animation_start(&a, 255, 400, 0);
    check("disabled animations are instant", animation_value(&a, 400), 255);
    check("instant animations are idle", animation_running(&a, 400), 0);
    animation_start(&a, 0, 450, 200);
    animation_set(&a, 0);
    check("settings/recovery settle the effect", animation_value(&a, 451), 0);
    check("settled effect is idle", animation_running(&a, 451), 0);
    monotonic(0, 255, 100, 200);
    monotonic(255, 0, 100, 240);
    monotonic(-300, 1920, 100, 180);
    monotonic(0, 255, 0xFFFFFFA0u, 220);   /* crosses the 32-bit timer wrap */
    monotonic(0, 255, 0, 1);
    monotonic(255, 255, 100, 200);
    check("lerp first endpoint", animation_lerp(-8, 800, 0), -8);
    check("lerp last endpoint", animation_lerp(-8, 800, 255), 800);
    check("lerp reverse endpoint", animation_lerp(1920, -8, 255), -8);
    printf("animation tests: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
