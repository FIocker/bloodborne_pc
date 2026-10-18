/* SPDX-License-Identifier: GPL-3.0-or-later
 * bbport: the direct mouse camera's step (src/mouse_camera_step.h, defined in
 * src/runtime_mouse_camera.c) on the host CPU - no guest, no Vulkan, no window.
 *
 * The step is where a mouse count becomes a camera angle, and it is the part a frame-rate
 * change must not alter: it takes no time argument at all, so the tests below pin the angle a
 * count turns (one big sample and the same counts split over many updates are the same turn),
 * the axis signs, the pitch/yaw bounds, the stick's priority, the drop when the camera is off,
 * and the counts a locked-on camera routes to the lock-on flick.
 *
 * Build (MSYS2 CLANG64, the toolchain build.sh uses on Windows):
 *   clang -std=c11 -O2 -g -Wall -Wextra -Werror -I. -Isrc tests/test_direct_mouse.c \
 *         src/runtime_mouse_camera.c -lm -o out/direct-mouse-test
 * runtime_mouse_camera.c carries its guest hooks as file-scope __asm__ blocks; they are linked
 * in (never called here) and need no flag beyond -lm. See docs/direct-input.md.
 */
/* <time.h>'s clock_gettime and nanosleep need this in a strict C11 build (the POSIX branch
 * below; the Windows build uses QueryPerformanceCounter/Sleep instead). */
#define _POSIX_C_SOURCE 200809L

#include "runtime_mouse_camera.h"

#include "mouse_camera_step.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* runtime_mouse_camera.c's own flag: the store/ramp stubs set it in the update where the free
 * camera ran, and mouse_camera_step_begin_update consumes it. */
extern uint8_t bb_mouse_free_ran;

/* mouse_direct_sensitivity = 5, the port's default: 0.25 degrees a count. */
#define PER_COUNT_025 0.25f
/* The mathematical pi: <math.h> defines M_PI only outside a strict C11 build. */
#define BB_PI 3.14159265358979323846
/* pi/180 as a float, the angle unit the step works in. */
#define DEGREES_TO_RADIANS (float)(BB_PI / 180.0)

static double seconds_now(void) {
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart ? (double)counter.QuadPart / (double)frequency.QuadPart : 0.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

/* The flick decision reads its own counts, so a test that starts from a known state has to
 * drain them: mouse_camera_step_begin_update is what fills them. */
static void drain_flick(void) {
    float dx = 0.0f, dy = 0.0f;
    uint32_t updates = 0;
    mouse_camera_step_take_flick_counts(&dx, &dy, &updates);
}

/* Every test starts from the same place: the camera active at the default sensitivity, no
 * inversion and no hold, no counts and no flick counts banked. */
static void reset_step(void) {
    mouse_camera_step_set_active(0);
    mouse_camera_step_begin_update(); /* the counts were dropped by set_active(0) already */
    drain_flick();
    mouse_camera_step_reset_stats();
    mouse_camera_step_publish(PER_COUNT_025, 0);
    bb_mouse_free_ran = 0;
    mouse_camera_step_set_active(1);
}

/* One update of counts, then the step that applies them; returns the update's turn. */
static int turn_one_update(float dx, float dy, float *pitch, float *yaw) {
    mouse_camera_step_add_counts(dx, dy);
    mouse_camera_step_begin_update();
    return mouse_camera_step_turn(0.0f, 0.0f, pitch, yaw, -1.0f, 1.0f);
}

/* DS3's own scale (the pad object's formula): 0.2 * (sensitivity * 0.15 + 0.5) degrees a
 * count, and the port's multipliers on top of it. */
static void test_degrees_per_count(void) {
    assert(fabsf(mouse_camera_degrees_per_count(0) - 0.1f) < 1e-6f);
    assert(fabsf(mouse_camera_degrees_per_count(5) - 0.25f) < 1e-6f);
    assert(fabsf(mouse_camera_degrees_per_count(10) - 0.4f) < 1e-6f);
    for (int s = 0; s < 10; ++s)
        assert(mouse_camera_degrees_per_count(s) < mouse_camera_degrees_per_count(s + 1));
    /* Out of range is clamped, not extrapolated. */
    assert(mouse_camera_degrees_per_count(-4) == mouse_camera_degrees_per_count(0));
    assert(mouse_camera_degrees_per_count(99) == mouse_camera_degrees_per_count(10));

    assert(fabsf(mouse_camera_degrees_per_count_scaled(5, 2.0f) - 0.5f) < 1e-6f);
    assert(fabsf(mouse_camera_degrees_per_count_scaled(5, 0.5f) - 0.125f) < 1e-6f);
    /* A multiplier outside (0, 100] is not a scale: the plain DS3 value is used. */
    assert(mouse_camera_degrees_per_count_scaled(5, 0.0f) == mouse_camera_degrees_per_count(5));
    assert(mouse_camera_degrees_per_count_scaled(5, -2.0f) == mouse_camera_degrees_per_count(5));
    assert(mouse_camera_degrees_per_count_scaled(5, 1000.0f) == mouse_camera_degrees_per_count(5));

    /* publish clamps the same way, so a bad value leaves the camera at the slow default. */
    reset_step();
    mouse_camera_step_publish(0.0f, 0);
    assert(fabsf(mouse_camera_step_degrees_per_count() - 0.1f) < 1e-6f);
    mouse_camera_step_publish(1.0e6f, 0);
    assert(fabsf(mouse_camera_step_degrees_per_count() - 0.1f) < 1e-6f);
    mouse_camera_step_publish(0.3f, 0);
    assert(fabsf(mouse_camera_step_degrees_per_count() - 0.3f) < 1e-6f);
    mouse_camera_step_publish(PER_COUNT_025, 0);
    puts("Direct mouse step: DS3's degrees a count and the port's multipliers PASS");
}

/* The step is an angle a count, so the counts are what decide the turn - never how they were
 * spread over updates. 40 counts in one update, 40 updates of one count and 10 updates of
 * four counts are the same camera angle. */
static void test_per_count_frame_independence(void) {
    float pitch = 0.0f, yaw = 0.0f;

    reset_step();
    assert(turn_one_update(40.0f, 0.0f, &pitch, &yaw) == 1);
    const float one_sample = yaw;
    assert(fabsf(one_sample - 40.0f * PER_COUNT_025 * DEGREES_TO_RADIANS) < 1e-6f);

    reset_step();
    pitch = yaw = 0.0f;
    for (int i = 0; i < 40; ++i) assert(turn_one_update(1.0f, 0.0f, &pitch, &yaw) == 1);
    assert(fabsf(yaw - one_sample) < 1e-6f);

    reset_step();
    pitch = yaw = 0.0f;
    for (int i = 0; i < 10; ++i) assert(turn_one_update(4.0f, 0.0f, &pitch, &yaw) == 1);
    assert(fabsf(yaw - one_sample) < 1e-6f);

    /* The pitch side of the same step: 25 counts once, then one count 25 times. */
    reset_step();
    pitch = yaw = 0.0f;
    assert(turn_one_update(0.0f, 25.0f, &pitch, &yaw) == 1);
    const float pitch_one_sample = pitch;
    assert(fabsf(pitch_one_sample - 25.0f * PER_COUNT_025 * DEGREES_TO_RADIANS) < 1e-6f);
    assert(fabsf(yaw) < 1e-9f);

    reset_step();
    pitch = yaw = 0.0f;
    for (int i = 0; i < 25; ++i) assert(turn_one_update(0.0f, 1.0f, &pitch, &yaw) == 1);
    assert(fabsf(pitch - pitch_one_sample) < 1e-6f);
    assert(fabsf(yaw) < 1e-9f);
    puts("Direct mouse step: a count is an angle, not a rate (one sample == many) PASS");
}

/* mouse_camera_invert_x / mouse_camera_invert_y flip one axis each, leaving the other alone. */
static void test_invert(void) {
    float pitch = 0.0f, yaw = 0.0f;

    reset_step();
    assert(turn_one_update(12.0f, 7.0f, &pitch, &yaw) == 1);
    const float yaw_plain = yaw, pitch_plain = pitch;
    /* DS3's signs: a count moves the camera the way the game's own stick step would. */
    assert(yaw_plain > 0.0f && pitch_plain > 0.0f);

    reset_step();
    mouse_camera_step_publish(PER_COUNT_025, MOUSE_CAMERA_INVERT_X);
    pitch = yaw = 0.0f;
    assert(turn_one_update(12.0f, 7.0f, &pitch, &yaw) == 1);
    assert(fabsf(yaw + yaw_plain) < 1e-6f);
    assert(fabsf(pitch - pitch_plain) < 1e-6f);

    reset_step();
    mouse_camera_step_publish(PER_COUNT_025, MOUSE_CAMERA_INVERT_Y);
    pitch = yaw = 0.0f;
    assert(turn_one_update(12.0f, 7.0f, &pitch, &yaw) == 1);
    assert(fabsf(pitch + pitch_plain) < 1e-6f);
    assert(fabsf(yaw - yaw_plain) < 1e-6f);

    reset_step();
    mouse_camera_step_publish(PER_COUNT_025, MOUSE_CAMERA_INVERT_X | MOUSE_CAMERA_INVERT_Y);
    pitch = yaw = 0.0f;
    assert(turn_one_update(12.0f, 7.0f, &pitch, &yaw) == 1);
    assert(fabsf(yaw + yaw_plain) < 1e-6f);
    assert(fabsf(pitch + pitch_plain) < 1e-6f);
    puts("Direct mouse step: each axis inverts on its own PASS");
}

/* The pitch stays inside the camera's own limits and the yaw stays inside one turn either
 * way, whatever the counts are. */
static void test_pitch_and_yaw_bounds(void) {
    float pitch = 0.0f, yaw = 0.0f;

    reset_step();
    mouse_camera_step_add_counts(0.0f, 100000.0f);
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_turn(0.0f, 0.0f, &pitch, &yaw, -0.6f, 0.4f) == 1);
    assert(pitch == 0.4f); /* the camera's own highest angle, +0x1ec */

    reset_step();
    pitch = yaw = 0.0f;
    mouse_camera_step_add_counts(0.0f, -100000.0f);
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_turn(0.0f, 0.0f, &pitch, &yaw, -0.6f, 0.4f) == 1);
    assert(pitch == -0.6f); /* the lowest, +0x1f0 */

    /* An angle already outside the limits is pulled back into them. */
    reset_step();
    pitch = 3.0f;
    yaw = 0.0f;
    mouse_camera_step_add_counts(1.0f, 0.0f);
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_turn(0.0f, 0.0f, &pitch, &yaw, -0.6f, 0.4f) == 1);
    assert(pitch == 0.4f);

    /* A degree a count: 360 counts is a full turn, which comes back to the same yaw. */
    reset_step();
    mouse_camera_step_publish(1.0f, 0);
    pitch = yaw = 0.0f;
    mouse_camera_step_add_counts(360.0f, 0.0f);
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_turn(0.0f, 0.0f, &pitch, &yaw, -1.0f, 1.0f) == 1);
    assert(fabsf(yaw) < 1e-4f);

    /* 3000 counts is 8.3 turns: the yaw comes back inside one turn, still the same angle
     * modulo a turn (the game reads its own yaw back each update, so it must stay bounded). */
    reset_step();
    mouse_camera_step_publish(1.0f, 0);
    pitch = yaw = 0.0f;
    mouse_camera_step_add_counts(3000.0f, 0.0f);
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_turn(0.0f, 0.0f, &pitch, &yaw, -1.0f, 1.0f) == 1);
    assert(isfinite(yaw) && fabsf(yaw) <= (float)BB_PI + 1e-4f);
    const float turns = (yaw - 3000.0f * DEGREES_TO_RADIANS) / (float)(2.0 * BB_PI);
    assert(fabsf(turns - roundf(turns)) < 1e-3f);

    /* The wrap is bounded at 64 turns an update: an absurd sample cannot spin the camera
     * forever (the loop cannot run away, and the residual is still a whole number of turns). */
    reset_step();
    mouse_camera_step_publish(1.0f, 0);
    pitch = yaw = 0.0f;
    mouse_camera_step_add_counts(100000.0f, 0.0f);
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_turn(0.0f, 0.0f, &pitch, &yaw, -1.0f, 1.0f) == 1);
    assert(isfinite(yaw));
    const float removed = (100000.0f * DEGREES_TO_RADIANS - yaw) / (float)(2.0 * BB_PI);
    assert(fabsf(removed - 64.0f) < 1e-2f);
    puts("Direct mouse step: the camera's own pitch limits and one turn of yaw PASS");
}

/* DS3's order: the stick turns the free camera first, and the mouse only when it did not. */
static void test_stick_priority(void) {
    float pitch = 0.0f, yaw = 0.0f;
    MouseCameraStepStats stats;

    reset_step();
    mouse_camera_step_add_counts(10.0f, 0.0f);
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_turn(0.5f, 0.0f, &pitch, &yaw, -1.0f, 1.0f) == 0);
    assert(pitch == 0.0f && yaw == 0.0f);
    assert(mouse_camera_step_frame()->turned == 0);
    mouse_camera_step_take_stats(&stats);
    assert(stats.stick_frames == 1 && stats.turns == 0);

    /* The counts the stick took are gone: the next update has none of its own. */
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_turn(0.0f, 0.0f, &pitch, &yaw, -1.0f, 1.0f) == 0);
    assert(yaw == 0.0f);

    /* A yaw-only stick step takes the camera the same way. */
    mouse_camera_step_add_counts(10.0f, 0.0f);
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_turn(0.0f, 0.25f, &pitch, &yaw, -1.0f, 1.0f) == 0);
    mouse_camera_step_take_stats(&stats);
    assert(stats.stick_frames == 2);

    /* A stick step with no mouse motion is not a "the stick had it" frame. */
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_turn(0.5f, 0.0f, &pitch, &yaw, -1.0f, 1.0f) == 0);
    mouse_camera_step_take_stats(&stats);
    assert(stats.stick_frames == 2);

    /* Stick released: the same counts do turn the camera. */
    mouse_camera_step_add_counts(10.0f, 0.0f);
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_turn(0.0f, 0.0f, &pitch, &yaw, -1.0f, 1.0f) == 1);
    assert(yaw > 0.0f);
    assert(mouse_camera_step_frame()->turned == 1);
    mouse_camera_step_take_stats(&stats);
    assert(stats.turns == 1 && stats.stick_frames == 2);
    puts("Direct mouse step: the stick keeps the camera while it turns it PASS");
}

/* Off (the F7 toggle, or mouse_camera = off) means the game's own camera is left alone at
 * once: motion is dropped where it arrives, and counts already in flight are dropped too. */
static void test_inactive_drops_counts(void) {
    float pitch = 0.0f, yaw = 0.0f, dx = 0.0f, dy = 0.0f;
    uint32_t updates = 0;
    MouseCameraStepStats stats;

    reset_step();
    mouse_camera_step_set_active(0);
    assert(mouse_camera_step_active() == 0);
    mouse_camera_step_add_counts(100.0f, 100.0f);
    mouse_camera_step_take_stats(&stats);
    assert(stats.counts == 0); /* never even counted */

    /* Nothing banked while off, so turning it on turns nothing. */
    mouse_camera_step_set_active(1);
    assert(mouse_camera_step_active() == 1);
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_turn(0.0f, 0.0f, &pitch, &yaw, -1.0f, 1.0f) == 0);
    assert(pitch == 0.0f && yaw == 0.0f);
    /* Nothing reached the lock-on switch either: the update counter counts the updates the
     * free camera did not run in, the counts are what a flick reads. */
    mouse_camera_step_take_flick_counts(&dx, &dy, &updates);
    assert(dx == 0.0f && dy == 0.0f && updates == 1);

    /* Counts in flight when it goes off are dropped, not kept for the next update. */
    mouse_camera_step_add_counts(50.0f, 0.0f);
    mouse_camera_step_set_active(0);
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_frame()->dx == 0.0f && mouse_camera_step_frame()->dy == 0.0f);
    assert(mouse_camera_step_turn(0.0f, 0.0f, &pitch, &yaw, -1.0f, 1.0f) == 0);
    assert(yaw == 0.0f);
    mouse_camera_step_take_flick_counts(&dx, &dy, &updates);
    assert(dx == 0.0f && dy == 0.0f && updates == 1);

    /* Counts already routed to the lock-on switch are dropped with the rest: off leaves the
     * game's camera alone, and nothing may switch its target later. */
    mouse_camera_step_set_active(1);
    mouse_camera_step_add_counts(300.0f, 0.0f);
    mouse_camera_step_begin_update();
    mouse_camera_step_take_flick_counts(&dx, &dy, &updates);
    assert(dx == 300.0f && updates == 1);
    mouse_camera_step_add_counts(300.0f, 0.0f);
    mouse_camera_step_begin_update();
    mouse_camera_step_set_active(0);
    mouse_camera_step_take_flick_counts(&dx, &dy, &updates);
    assert(dx == 0.0f && dy == 0.0f && updates == 0);
    puts("Direct mouse step: off drops the motion in flight PASS");
}

/* Locked on, the game drives the camera: the counts of the updates the free camera did not
 * run in are what the flick decision reads. */
static void test_lock_on_count_routing(void) {
    float dx = 0.0f, dy = 0.0f;
    uint32_t updates = 0;

    reset_step();

    /* The free camera ran in the update before this one: the counts are the camera's. */
    mouse_camera_step_add_counts(9.0f, -4.0f);
    mouse_camera_step_free_ran();
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_frame()->free_before == 1);
    assert(mouse_camera_step_frame()->dx == 9.0f && mouse_camera_step_frame()->dy == -4.0f);
    mouse_camera_step_take_flick_counts(&dx, &dy, &updates);
    assert(dx == 0.0f && dy == 0.0f && updates == 0);

    /* The flag is one update's, not sticky. */
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_frame()->free_before == 0);

    /* No free camera last update: the counts are what a flick's target switch reads. */
    drain_flick();
    mouse_camera_step_add_counts(9.0f, -4.0f);
    mouse_camera_step_begin_update();
    mouse_camera_step_take_flick_counts(&dx, &dy, &updates);
    assert(dx == 9.0f && dy == -4.0f && updates == 1);

    /* Taken, not copied. */
    mouse_camera_step_take_flick_counts(&dx, &dy, &updates);
    assert(dx == 0.0f && dy == 0.0f && updates == 0);

    /* Counts accumulate over the locked-on updates until the pad read takes them. */
    for (int i = 0; i < 5; ++i) {
        mouse_camera_step_add_counts(2.0f, 1.0f);
        mouse_camera_step_begin_update();
    }
    mouse_camera_step_take_flick_counts(&dx, &dy, &updates);
    assert(dx == 10.0f && dy == 5.0f && updates == 5);

    /* A free-camera update in between stops the routing for that update only. */
    mouse_camera_step_add_counts(2.0f, 1.0f);
    mouse_camera_step_free_ran();
    mouse_camera_step_begin_update();
    mouse_camera_step_take_flick_counts(&dx, &dy, &updates);
    assert(dx == 0.0f && dy == 0.0f && updates == 0);
    mouse_camera_step_add_counts(2.0f, 1.0f);
    mouse_camera_step_begin_update();
    mouse_camera_step_take_flick_counts(&dx, &dy, &updates);
    assert(dx == 2.0f && dy == 1.0f && updates == 1);
    puts("Direct mouse step: a locked-on camera's counts route to the flick PASS");
}

/* The routing above only matters because the pad read turns it into a target switch: a fast
 * movement switches, a slow one rearms, and the cooldown and the hold bound the rate. */
static void test_lock_on_flick_gate(void) {
    float x = 0.0f, y = 0.0f;
    MouseCameraStepStats before, after;

    reset_step();
    assert(mouse_camera_flick(&x, &y) == 0); /* primes the rate window */

    /* Counts the free camera took never reach the switch, however fast they are. */
    sleep_ms(20);
    mouse_camera_step_add_counts(2000.0f, 0.0f);
    mouse_camera_step_free_ran();
    mouse_camera_step_begin_update();
    assert(mouse_camera_flick(&x, &y) == 0);

    /* A locked-on camera's fast movement switches the target, and reports it. */
    sleep_ms(20);
    mouse_camera_step_take_stats(&before);
    mouse_camera_step_add_counts(2000.0f, 0.0f);
    mouse_camera_step_begin_update();
    assert(mouse_camera_flick(&x, &y) == 1);
    assert(fabsf(x - 1.0f) < 1e-6f && fabsf(y) < 1e-6f);
    assert(lroundf(x * 127.0f) == 127); /* what runtime_pad.c sends the right stick */
    mouse_camera_step_take_stats(&after);
    assert(after.flicks == before.flicks + 1);
    const double flicked_at = seconds_now();

    /* The direction holds for its own 80 ms, so the game sees a stick flick. */
    assert(mouse_camera_flick(&x, &y) == 1);

    /* Slow movement never switches, and rearms the switch for the next fast one. */
    sleep_ms(150);
    mouse_camera_step_add_counts(100.0f, 0.0f);
    mouse_camera_step_begin_update();
    assert(mouse_camera_flick(&x, &y) == 0);

    /* Rearmed: the same fast movement in the other direction switches that way. */
    sleep_ms(20);
    mouse_camera_step_add_counts(-2000.0f, 0.0f);
    mouse_camera_step_begin_update();
    assert(mouse_camera_flick(&x, &y) == 1);
    assert(fabsf(x + 1.0f) < 1e-6f && fabsf(y) < 1e-6f);
    assert(lroundf(x * 127.0f) == -127);

    /* The 0.5 s cooldown keeps one movement from switching twice. */
    sleep_ms(150);
    mouse_camera_step_add_counts(-2000.0f, 0.0f);
    mouse_camera_step_begin_update();
    if (seconds_now() - flicked_at < 0.35) {
        assert(mouse_camera_flick(&x, &y) == 0);
    } else {
        puts("Direct mouse step: the flick cooldown check was skipped (the host stalled)");
    }
    puts("Direct mouse step: the lock-on flick's switch, hold, rearm and cooldown PASS");
}

/* The camera object the game's own update holds: pitch +0x140, yaw +0x144, the limits +0x1ec
 * (highest) and +0x1f0 (lowest), and the update's input byte set when the mouse turned it. */
static void test_turn_camera_object(void) {
    uint8_t cam[0x210];
    uint8_t input = 0;
    float pitch = 0.1f, yaw = 0.2f, lowest = -0.9f, highest = 0.5f;
    float got_pitch = 0.0f, got_yaw = 0.0f;
    memset(cam, 0, sizeof(cam));
    memcpy(cam + 0x140, &pitch, 4);
    memcpy(cam + 0x144, &yaw, 4);
    memcpy(cam + 0x1ec, &highest, 4);
    memcpy(cam + 0x1f0, &lowest, 4);

    reset_step();
    mouse_camera_step_add_counts(8.0f, 0.0f);
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_turn_camera(cam, &input, 0.0f, 0.0f) == 1);
    memcpy(&got_pitch, cam + 0x140, 4);
    memcpy(&got_yaw, cam + 0x144, 4);
    assert(fabsf(got_pitch - pitch) < 1e-7f);
    assert(fabsf(got_yaw - (yaw + 8.0f * PER_COUNT_025 * DEGREES_TO_RADIANS)) < 1e-6f);
    assert(input == 1);

    /* The object's own limits are the pitch clamp. */
    reset_step();
    mouse_camera_step_add_counts(0.0f, 100000.0f);
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_turn_camera(cam, &input, 0.0f, 0.0f) == 1);
    memcpy(&got_pitch, cam + 0x140, 4);
    assert(got_pitch == highest);

    /* The stick has it: neither angle nor the input byte changes. */
    reset_step();
    input = 0;
    memcpy(cam + 0x140, &pitch, 4);
    memcpy(cam + 0x144, &yaw, 4);
    mouse_camera_step_add_counts(8.0f, 8.0f);
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_turn_camera(cam, &input, 0.5f, 0.0f) == 0);
    memcpy(&got_pitch, cam + 0x140, 4);
    memcpy(&got_yaw, cam + 0x144, 4);
    assert(got_pitch == pitch && got_yaw == yaw && input == 0);

    /* No input byte to set: the angles are still written. */
    reset_step();
    mouse_camera_step_add_counts(8.0f, 0.0f);
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_turn_camera(cam, NULL, 0.0f, 0.0f) == 1);
    memcpy(&got_yaw, cam + 0x144, 4);
    assert(fabsf(got_yaw - (yaw + 8.0f * PER_COUNT_025 * DEGREES_TO_RADIANS)) < 1e-6f);
    puts("Direct mouse step: the game's camera object takes the angles and the input byte PASS");
}

/* The published flags and the counters the trace and the exit report print. */
static void test_published_flags_and_stats(void) {
    MouseCameraStepStats stats;

    reset_step();
    mouse_camera_step_publish(PER_COUNT_025, MOUSE_CAMERA_HOLD_AUTO_ROTATION);
    assert(mouse_camera_step_flags() == MOUSE_CAMERA_HOLD_AUTO_ROTATION);
    assert(mouse_camera_step_held() == 0); /* published, not yet an update */
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_held() == 1);
    assert(mouse_camera_step_frame()->hold == 1);
    mouse_camera_step_take_stats(&stats);
    assert(stats.updates == 1 && stats.held == 1 && stats.turns == 0);

    /* Every sample with motion is one count sample; a zero sample is not. */
    mouse_camera_step_reset_stats();
    mouse_camera_step_add_counts(1.0f, 0.0f);
    mouse_camera_step_add_counts(0.0f, 0.0f);
    mouse_camera_step_add_counts(-3.0f, 2.0f);
    mouse_camera_step_take_stats(&stats);
    assert(stats.counts == 2);

    /* reset_stats clears the flick counter too: a stale count would outlive the run it
     * describes in the exit report. */
    mouse_camera_step_report_flick();
    mouse_camera_step_take_stats(&stats);
    assert(stats.flicks == 1);
    mouse_camera_step_reset_stats();
    mouse_camera_step_take_stats(&stats);
    assert(stats.flicks == 0);

    /* The frame is the update's own: the counts, and what the hooks did with them. */
    mouse_camera_step_begin_update();
    assert(mouse_camera_step_frame()->dx == -2.0f && mouse_camera_step_frame()->dy == 2.0f);
    assert(mouse_camera_step_frame()->free_before == 0);
    /* turn() is what writes "the mouse turned the camera in this update". */
    float pitch = 0.0f, yaw = 0.0f;
    assert(mouse_camera_step_turn(0.0f, 0.0f, &pitch, &yaw, -1.0f, 1.0f) == 1);
    assert(mouse_camera_step_frame()->turned == 1);
    mouse_camera_step_take_stats(&stats);
    assert(stats.turns == 1);
    puts("Direct mouse step: published flags and the statistics PASS");
}

int main(void) {
    test_degrees_per_count();
    test_per_count_frame_independence();
    test_invert();
    test_pitch_and_yaw_bounds();
    test_stick_priority();
    test_inactive_drops_counts();
    test_lock_on_count_routing();
    test_lock_on_flick_gate();
    test_turn_camera_object();
    test_published_flags_and_stats();
    puts("Direct mouse camera step: all tests passed");
    return 0;
}
