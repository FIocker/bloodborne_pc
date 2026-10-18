// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/menu_pointer.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
int main() {
    float x,y;
    assert(menu_pointer_stage(960,540,1920,1080,1920,1080,&x,&y));
    assert(x==960 && y==540);
    assert(menu_pointer_stage(1720,720,3440,1440,3440,1440,&x,&y));
    assert(std::abs(x-960)<.001 && std::abs(y-540)<.001);
    assert(!menu_pointer_stage(0,720,3440,1440,3440,1440,&x,&y));
    assert(menu_pointer_stage(960,540,1920,1080,3440,1440,&x,&y));
    assert(std::abs(x-960)<.001 && std::abs(y-540)<.001);
    assert(!menu_pointer_stage(960,0,1920,1080,3440,1440,&x,&y));
    assert(!menu_pointer_stage(-1,540,1920,1080,1920,1080,&x,&y));
    assert(!menu_pointer_stage(1920,540,1920,1080,1920,1080,&x,&y));
    assert(!menu_pointer_stage(std::numeric_limits<float>::quiet_NaN(),0,1920,1080,1920,1080,&x,&y));
    assert(!menu_pointer_stage(0,0,0,1080,1920,1080,&x,&y));
    assert(!menu_pointer_stage(0,0,1920,1080,1920,1080,nullptr,&y));
    assert(!menu_pointer_installed());
    menu_pointer_bind_image(nullptr,0);
    assert(!menu_pointer_installed() && !menu_pointer_in_menu());
    menu_pointer_scroll(0,1); menu_pointer_back();
    assert(menu_pointer_take_press()==0);
    puts("PASS: native and resized ultrawide coordinates, letterbox/HUD margins, invalid inputs, uninstalled pointer");
}
