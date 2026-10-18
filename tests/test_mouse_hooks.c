/* SPDX-License-Identifier: GPL-3.0-or-later
 * Execute the actual camera trampolines on a synthetic 1.09 image, without game assets. */
#include "../src/runtime_mouse_camera.c"
#include <assert.h>
#ifdef _WIN32
static void populate(uint8_t *image) {
    g_image=image;
    memcpy(guest_ptr(GUEST_UPDATE),kUpdateBytes,sizeof(kUpdateBytes));
    memcpy(guest_ptr(GUEST_STORE),kStoreBytes,sizeof(kStoreBytes));
    memcpy(guest_ptr(GUEST_RAMP1),kRampBytes[0],14);
    memcpy(guest_ptr(GUEST_RAMP2),kRampBytes[1],14);
    for(int i=0;i<4;++i) memcpy(guest_ptr(kHoldSites[i]),kHoldBytes[i],9);
}
/* Same saved-register/stack unwind as the displaced update prologue. */
static const uint8_t finish_update[]={0x48,0x81,0xc4,0xd8,0x03,0,0,
    0x5b,0x41,0x5c,0x41,0x5d,0x41,0x5e,0x41,0x5f,0x5d,0xc3};
extern BB_GUEST_ABI uint64_t test_store(uint8_t *cam,uint8_t *input);
__asm__(".text\n.globl test_store\ntest_store:\n"
        "push %r13\npush %rbp\nmov %rdi,%r13\nlea 0x3c4(%rsi),%rbp\n"
        "vxorps %xmm0,%xmm0,%xmm0\nvxorps %xmm1,%xmm1,%xmm1\n"
        "vxorps %xmm10,%xmm10,%xmm10\nstc\ncall bb_mouse_store_stub\nsetc %al\nmovzbl %al,%eax\n"
        "pop %rbp\npop %r13\nret\n");
int main(void) {
    const size_t bytes=0x1600000;
    uint8_t *image=VirtualAlloc(NULL,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    assert(image); populate(image);
    _putenv_s("BB_INPUT_EBOOT_SEGMENTS","unknown");
    mouse_camera_bind_image(image,bytes);
    assert(!mouse_camera_installed() && !memcmp(guest_ptr(GUEST_UPDATE),kUpdateBytes,20));
    g_setup_failed=0;
    _putenv_s("BB_INPUT_EBOOT_SEGMENTS",kEboot109Fingerprint);
    ((uint8_t*)guest_ptr(GUEST_RAMP2))[0]^=1;
    mouse_camera_bind_image(image,bytes);
    assert(!mouse_camera_installed() && !memcmp(guest_ptr(GUEST_UPDATE),kUpdateBytes,20));
    g_setup_failed=0; populate(image);
    memcpy(guest_ptr(GUEST_UPDATE)+20,finish_update,sizeof(finish_update));
    ((uint8_t*)guest_ptr(GUEST_RESUME))[0]=0xc3;
    mouse_camera_bind_image(image,bytes);
    assert(mouse_camera_installed());
    assert(bb_mouse_store_resume==guest_ptr(GUEST_RESUME));
    assert(bb_mouse_update_resume==guest_ptr(GUEST_UPDATE)+20);
    assert(bb_mouse_store_resume!=bb_mouse_update_resume);
    mouse_camera_set_enabled(1);
    mouse_camera_step_publish(.25f,0);
    mouse_camera_step_add_counts(40,20);
    ((void (BB_GUEST_ABI *)(void))guest_ptr(GUEST_UPDATE))();
    MouseCameraStepStats stats; mouse_camera_step_take_stats(&stats);
    assert(stats.updates==1);
    uint8_t cam[0x200]={0},input=0;
    float low=-1,high=1;
    memcpy(cam+0x1ec,&high,4); memcpy(cam+0x1f0,&low,4);
    assert(test_store(cam,&input)==1); // The displaced stores leave guest flags intact.
    float pitch,yaw; memcpy(&pitch,cam+0x140,4); memcpy(&yaw,cam+0x144,4);
    assert(input==1 && fabsf(pitch-5*.0174532924f)<1e-6f);
    assert(fabsf(yaw-10*.0174532924f)<1e-6f);
    mouse_camera_set_enabled(0);
    VirtualFree(image,0,MEM_RELEASE);
    puts("PASS: fingerprint/byte guards, actual SysV update/store trampolines, distinct resumes");
    return 0;
}
#else
int main(void) { puts("SKIP: Windows trampoline test"); return 0; }
#endif
