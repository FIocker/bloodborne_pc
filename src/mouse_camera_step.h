/* SPDX-License-Identifier: GPL-3.0-or-later
 * bbport: the mouse's part of the follow camera's step, ported from bbhost
 * (droogie/bbhost src/engine/mouse_camera_step.h, 566184a). The window adds the mouse's
 * counts, the pad read publishes the settings, and the camera takes the counts at the start
 * of its update and turns by them after the stick - DS3's order. Nothing here calls the host
 * or takes a lock, so the camera's own thread and a hook in its code can both use it. */
#ifndef BBPORT_MOUSE_CAMERA_STEP_H
#define BBPORT_MOUSE_CAMERA_STEP_H

#include <stdint.h>

/* Published by the pad read on every sample (runtime_pad.c). */
enum {
    MOUSE_CAMERA_INVERT_X = 1u << 0,
    MOUSE_CAMERA_INVERT_Y = 1u << 1,
    /* The camera's own turns are held: the mouse drives the camera and the
     * auto-rotation setting says so. */
    MOUSE_CAMERA_HOLD_AUTO_ROTATION = 1u << 2
};

/* The camera update, as the camera's own thread sees it. */
typedef struct {
    float dx, dy;     /* this update's counts, taken by mouse_camera_step_begin_update */
    int free_before;  /* the free camera ran in the update before this one */
    int hold;         /* auto-rotation is held in this update */
    int turned;       /* the mouse turned the camera in this update */
} MouseCameraStepFrame;

typedef struct {
    uint64_t updates, turns, stick_frames, held, counts, flicks;
} MouseCameraStepStats;

/* DS3's degrees a count at a sensitivity of 0..10: 0.2 * (s * 0.15 + 0.5). */
float mouse_camera_degrees_per_count(int sensitivity);
/* The same, with the port's own multiplier on top (input.ini's
 * mouse_direct_sensitivity/mouse_camera_sensitivity/mouse_camera_scale). */
float mouse_camera_degrees_per_count_scaled(int sensitivity, float scale);

/* The pad read: the step to apply, and the flags above. */
void mouse_camera_step_publish(float degrees_per_count, uint32_t flags);
uint32_t mouse_camera_step_flags(void);
float mouse_camera_step_degrees_per_count(void);

/* The window (or its sampler thread): relative counts to turn the camera by, and whether the
 * direct camera is the mouse's route at all. Motion added while it is not is dropped, so
 * nothing banks while the mouse is a pointer or the camera mode is off. */
void mouse_camera_step_add_counts(float dx, float dy);
void mouse_camera_step_set_active(int active);
int mouse_camera_step_active(void);

/* The camera's update, first thing (the prologue hook): this update's counts, the free
 * camera's state, and the counts a flick's target switch reads. */
void mouse_camera_step_begin_update(void);
/* The free camera ran in this update (the store and fast-turn hooks). */
void mouse_camera_step_free_ran(void);
int mouse_camera_step_held(void);
const MouseCameraStepFrame *mouse_camera_step_frame(void);

/* DS3's step, after the stick has turned the free camera: when the stick has not, the pitch
 * and yaw turn by this update's counts - an angle a count, no frame time, no dead zone, no
 * cap - the pitch held within the stick step's limits and the yaw kept within a turn either
 * way. Returns whether the mouse turned the camera. */
int mouse_camera_step_turn(float stick_pitch, float stick_yaw, float *pitch, float *yaw,
                           float pitch_min, float pitch_max);
/* The same, on the game's camera object: pitch +0x140, yaw +0x144, its limits +0x1ec (the
 * highest) and +0x1f0 (the lowest), and the update's input byte set when the mouse turned it. */
int mouse_camera_step_turn_camera(uint8_t *cam, uint8_t *input, float stick_pitch, float stick_yaw);

/* The counts of updates that came after one without the free camera - lock-on, or the game
 * turning it - and how many such updates, drained by the flick decision below. */
void mouse_camera_step_take_flick_counts(float *dx, float *dy, uint32_t *updates);
void mouse_camera_step_report_flick(void);
void mouse_camera_step_take_stats(MouseCameraStepStats *out);
void mouse_camera_step_reset_stats(void);

#endif
