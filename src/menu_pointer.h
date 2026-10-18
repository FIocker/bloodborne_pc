/* SPDX-License-Identifier: GPL-3.0-or-later
 * Game-menu hit testing adapted from droogie/bbhost (566184a). */
#ifndef BBPORT_MENU_POINTER_H
#define BBPORT_MENU_POINTER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void menu_pointer_bind_image(void *image, uint64_t size);
int menu_pointer_installed(void);
int menu_pointer_in_menu(void);
void menu_pointer_click(void);
uint32_t menu_pointer_take_press(void);
uint32_t menu_region_confirm_button(void);
uint32_t menu_confirm_button(void);
uint32_t menu_back_button(void);
uint64_t menu_pointer_tick(void);
/* Window thread: client pixels and image/output sizes, plus focus/capture gating. */
void menu_pointer_position(float x, float y, int window_w, int window_h,
                           int output_w, int output_h, int focused, int relative, int blocked);
/* Actual input events select ownership; resting controllers never steal the mouse. */
void menu_pointer_device(int controller);
int menu_pointer_using_controller(void);
void menu_pointer_scroll(int x, int y);
void menu_pointer_back(void);
void menu_pointer_pad(uint32_t buttons, uint8_t lx, uint8_t ly);
/* Pure coordinate conversion shared by native tests and the window bridge. */
int menu_pointer_stage(float x, float y, int window_w, int window_h,
                       int output_w, int output_h, float *stage_x, float *stage_y);
#ifdef __cplusplus
}
#endif
#endif
