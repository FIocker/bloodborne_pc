/* SPDX-License-Identifier: GPL-3.0-or-later
 * bbport: the direct mouse camera (input.ini's mouse_camera = direct), ported from bbhost
 * droogie/bbhost src/engine/mouse_camera.cpp (566184a), whose hooks in the game's follow
 * camera and interpretation of Dark Souls III's mouse camera it follows. See
 * docs/direct-input.md. */
#ifndef BBPORT_RUNTIME_MOUSE_CAMERA_H
#define BBPORT_RUNTIME_MOUSE_CAMERA_H

#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* probe.c, once the image is loaded and patched and before the guest runs: places the hooks
 * when the eboot is the 1.09 build the guard names. A refusal leaves the port on the stick
 * path and is reported once. */
void mouse_camera_bind_image(void *image, uint64_t size);

/* 1 when the follow camera's code was found and hooked: the direct camera can work. */
int mouse_camera_installed(void);
/* 1 while the mouse drives the camera: installed, wanted by input.ini, and not turned off by
 * the hotkey (mouse_camera_set_enabled). */
int mouse_camera_active(void);
/* The window thread: input.ini's mode and the F7-style toggle. Off drops any counts in
 * flight, so the game's own camera is left alone the moment it is off. */
void mouse_camera_set_enabled(int enabled);
/* The window (or its sampler) thread: the mouse's relative motion this sample. */
void mouse_camera_motion(float dx, float dy);

/* The pad read, every sample: DS3's sensitivity (0..10), the port's two multipliers,
 * the axes' inversion, and whether the camera's own turns are held for the mouse. */
void mouse_camera_set_vertical_scale(float scale);
void mouse_camera_publish(int sensitivity, float sensitivity_multiplier, float scale,
                          int invert_x, int invert_y, int hold_auto_rotation);

/* The pad read: a lock-on flick's direction (-1..1, x right, y down); 1 while one shows. */
int mouse_camera_flick(float *x, float *y);

/* BB_MOUSE_TRACE=1: one throttled line a second with the mouse's counts, whether they went to
 * the camera or to the stick, and the hook's state. Default: silent. */
void mouse_camera_trace(const char *route, float dx, float dy, int stick_x, int stick_y);
/* The exit report: what the mouse turned, and how often. */
void mouse_camera_report(void);

/* 1 when BB_MOUSE_TRACE=1 asked for the diagnostics. */
int mouse_camera_trace_on(void);

#ifdef __cplusplus
}
#endif
#endif
