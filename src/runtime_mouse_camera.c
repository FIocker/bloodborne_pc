/* SPDX-License-Identifier: GPL-3.0-or-later
 * bbport: the direct mouse camera - input.ini's mouse_camera = direct - ported from bbhost
 * droogie/bbhost src/engine/mouse_camera.cpp, src/engine/mouse_camera_step.h and
 * src/engine/mouse_camera_stubs.cpp (566184a). Dark Souls III's mouse camera is an angle per
 * count of the mouse's own movement, added to the follow camera's pitch and yaw once a frame,
 * after the stick and only when the stick has not turned it: no frame time, no dead zone, no
 * cap and no acceleration. Bloodborne's ChrExFollowCam::Update is the same function without
 * the mouse, so the step goes where the stick's own step stores the free camera's angles, and
 * the update's prologue takes the counts. Locked on, the game drives the camera and the pad
 * read turns a fast flick into the right stick the lock-on switch already reads. While the
 * mouse drives the camera, the game's own turns as the character moves are held, unless the
 * setting says otherwise (mouse_hold_auto_rotation).
 *
 * The hooks are placed only on the verified Bloodborne 1.09 image (the same segment
 * fingerprint the aspect-ratio hooks use) and only after every instruction they displace has
 * been read back and matched; anything else logs one line and leaves the port on the
 * mouse-to-stick path. See docs/direct-input.md. */
#include "runtime_mouse_camera.h"

#include "mouse_camera_step.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif

/* The guest runs the System V ABI; the port's own code is not always built for it. */
#ifdef _WIN32
#define BB_GUEST_ABI __attribute__((sysv_abi))
#else
#define BB_GUEST_ABI
#endif

/* ---------------------------------------------------------------- the step */

/* DS3's degrees a count at a sensitivity of 0..10 (its pad object's own formula). */
float mouse_camera_degrees_per_count(int sensitivity) {
    if (sensitivity < 0) sensitivity = 0;
    if (sensitivity > 10) sensitivity = 10;
    return ((sensitivity * 0.1f) * 1.5f + 0.5f) * 0.2f;
}

float mouse_camera_degrees_per_count_scaled(int sensitivity, float scale) {
    if (!(scale > 0.0f) || scale > 100.0f) scale = 1.0f;
    return mouse_camera_degrees_per_count(sensitivity) * scale;
}

static _Atomic uint32_t g_vertical_scale = 0x3f800000u;
void mouse_camera_set_vertical_scale(float scale) {
    if (!isfinite(scale) || scale < 0.1f || scale > 10.0f) scale=1.0f;
    uint32_t bits; memcpy(&bits,&scale,4);
    atomic_store_explicit(&g_vertical_scale,bits,memory_order_relaxed);
}

static uint64_t pack2(float x, float y) {
    uint32_t a, b;
    memcpy(&a, &x, 4);
    memcpy(&b, &y, 4);
    return (uint64_t)a | ((uint64_t)b << 32);
}

static void unpack2(uint64_t v, float *x, float *y) {
    uint32_t a = (uint32_t)v, b = (uint32_t)(v >> 32);
    memcpy(x, &a, 4);
    memcpy(y, &b, 4);
}

/* The window's relative motion while the mouse turns the camera, since the camera last took
 * it, and the settings the pad read published. */
static atomic_uint_fast64_t g_counts;
static atomic_uint g_degrees_bits;
static atomic_uint g_flags;
static atomic_int g_step_active;

/* The camera's thread, and the counts a flick's switch reads. */
static MouseCameraStepFrame g_frame;
static atomic_uint_fast64_t g_flick_counts;
static atomic_uint g_flick_updates;
extern uint8_t bb_mouse_free_ran;

static atomic_uint_fast64_t g_updates, g_turns, g_stick_frames, g_held, g_count_total, g_flicks;

static void step_add(atomic_uint_fast64_t *to, float x, float y) {
    uint64_t was = atomic_load_explicit(to, memory_order_relaxed);
    for (;;) {
        float ox, oy;
        unpack2(was, &ox, &oy);
        if (atomic_compare_exchange_weak_explicit(to, &was, pack2(ox + x, oy + y),
                                                  memory_order_acq_rel, memory_order_relaxed)) {
            return;
        }
    }
}

void mouse_camera_step_publish(float degrees_per_count, uint32_t flags) {
    uint32_t bits;
    if (!(degrees_per_count > 0.0f) || degrees_per_count > 100.0f) degrees_per_count = 0.1f;
    memcpy(&bits, &degrees_per_count, 4);
    atomic_store_explicit(&g_degrees_bits, bits, memory_order_relaxed);
    atomic_store_explicit(&g_flags, flags, memory_order_relaxed);
}

uint32_t mouse_camera_step_flags(void) { return atomic_load_explicit(&g_flags, memory_order_relaxed); }

float mouse_camera_step_degrees_per_count(void) {
    uint32_t bits = atomic_load_explicit(&g_degrees_bits, memory_order_relaxed);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

void mouse_camera_step_set_active(int active) {
    atomic_store_explicit(&g_step_active, active ? 1 : 0, memory_order_release);
    if (!active) {
        atomic_store_explicit(&g_counts, 0, memory_order_release);
        atomic_store_explicit(&g_flick_counts, 0, memory_order_release);
        atomic_store_explicit(&g_flick_updates, 0, memory_order_release);
    }
}

int mouse_camera_step_active(void) { return atomic_load_explicit(&g_step_active, memory_order_acquire); }

void mouse_camera_step_add_counts(float dx, float dy) {
    if (dx == 0.0f && dy == 0.0f) return;
    if (!mouse_camera_step_active()) return;
    step_add(&g_counts, dx, dy);
    atomic_fetch_add_explicit(&g_count_total, 1, memory_order_relaxed);
}

void mouse_camera_step_begin_update(void) {
    atomic_fetch_add_explicit(&g_updates, 1, memory_order_relaxed);
    float dx = 0.0f, dy = 0.0f;
    unpack2(atomic_exchange_explicit(&g_counts, 0, memory_order_acq_rel), &dx, &dy);
    g_frame.dx = dx;
    g_frame.dy = dy;
    g_frame.hold = (atomic_load_explicit(&g_flags, memory_order_relaxed) &
                    MOUSE_CAMERA_HOLD_AUTO_ROTATION) != 0;
    if (g_frame.hold) atomic_fetch_add_explicit(&g_held, 1, memory_order_relaxed);
    g_frame.free_before = __atomic_load_n(&bb_mouse_free_ran, __ATOMIC_RELAXED) != 0;
    __atomic_store_n(&bb_mouse_free_ran, 0, __ATOMIC_RELAXED);
    if (!g_frame.free_before) {
        if (dx != 0.0f || dy != 0.0f) step_add(&g_flick_counts, dx, dy);
        atomic_fetch_add_explicit(&g_flick_updates, 1, memory_order_relaxed);
    }
}

void mouse_camera_step_free_ran(void) { __atomic_store_n(&bb_mouse_free_ran, 1, __ATOMIC_RELAXED); }

int mouse_camera_step_held(void) { return g_frame.hold; }

const MouseCameraStepFrame *mouse_camera_step_frame(void) { return &g_frame; }

int mouse_camera_step_turn(float stick_pitch, float stick_yaw, float *pitch, float *yaw,
                           float pitch_min, float pitch_max) {
    const float dx = g_frame.dx, dy = g_frame.dy;
    g_frame.turned = 0;
    if (!mouse_camera_step_active()) return 0;
    if (stick_pitch != 0.0f || stick_yaw != 0.0f) {
        /* The stick has the camera this update, as in DS3. */
        if (dx != 0.0f || dy != 0.0f) atomic_fetch_add_explicit(&g_stick_frames, 1, memory_order_relaxed);
        return 0;
    }
    if (dx == 0.0f && dy == 0.0f) return 0;
    const float radians_per_degree = 0.0174532924f; /* DS3's constant, the float of pi/180 */
    const float pi = 3.14159274f, two_pi = 6.28318548f;
    const uint32_t flags = atomic_load_explicit(&g_flags, memory_order_relaxed);
    const float per_count = mouse_camera_step_degrees_per_count();
    /* DS3's signs: a count is negated unless the axis is inverted, and the camera subtracts. */
    uint32_t vertical_bits=atomic_load_explicit(&g_vertical_scale,memory_order_relaxed);
    float vertical_scale; memcpy(&vertical_scale,&vertical_bits,4);
    const float mouse_pitch = ((flags & MOUSE_CAMERA_INVERT_Y) ? 1.0f : -1.0f) * dy * per_count * vertical_scale;
    const float mouse_yaw = ((flags & MOUSE_CAMERA_INVERT_X) ? 1.0f : -1.0f) * dx * per_count;
    float p = *pitch - mouse_pitch * radians_per_degree;
    float y = *yaw - mouse_yaw * radians_per_degree;
    if (p < pitch_min) p = pitch_min;
    if (p > pitch_max) p = pitch_max;
    /* The game reads the yaw back from where the camera is each update, which bounds it; while
     * auto-rotation is held it does not, so the mouse bounds it here. A turn a step: no mouse
     * turns the camera 64 times in one update. */
    for (int i = 0; i < 64 && y > pi; ++i) y -= two_pi;
    for (int i = 0; i < 64 && y < -pi; ++i) y += two_pi;
    *pitch = p;
    *yaw = y;
    g_frame.turned = 1;
    atomic_fetch_add_explicit(&g_turns, 1, memory_order_relaxed);
    return 1;
}

int mouse_camera_step_turn_camera(uint8_t *cam, uint8_t *input, float stick_pitch, float stick_yaw) {
    float pitch, yaw, lowest, highest;
    memcpy(&pitch, cam + 0x140, 4);
    memcpy(&yaw, cam + 0x144, 4);
    memcpy(&highest, cam + 0x1ec, 4);
    memcpy(&lowest, cam + 0x1f0, 4);
    if (!mouse_camera_step_turn(stick_pitch, stick_yaw, &pitch, &yaw, lowest, highest)) return 0;
    memcpy(cam + 0x140, &pitch, 4);
    memcpy(cam + 0x144, &yaw, 4);
    if (input) *input = 1;
    return 1;
}

void mouse_camera_step_take_flick_counts(float *dx, float *dy, uint32_t *updates) {
    unpack2(atomic_exchange_explicit(&g_flick_counts, 0, memory_order_acq_rel), dx, dy);
    *updates = atomic_exchange_explicit(&g_flick_updates, 0, memory_order_acq_rel);
}

void mouse_camera_step_report_flick(void) { atomic_fetch_add_explicit(&g_flicks, 1, memory_order_relaxed); }

void mouse_camera_step_take_stats(MouseCameraStepStats *out) {
    out->updates = atomic_load_explicit(&g_updates, memory_order_relaxed);
    out->turns = atomic_load_explicit(&g_turns, memory_order_relaxed);
    out->stick_frames = atomic_load_explicit(&g_stick_frames, memory_order_relaxed);
    out->held = atomic_load_explicit(&g_held, memory_order_relaxed);
    out->counts = atomic_load_explicit(&g_count_total, memory_order_relaxed);
    out->flicks = atomic_load_explicit(&g_flicks, memory_order_relaxed);
}

void mouse_camera_step_reset_stats(void) {
    atomic_store_explicit(&g_flicks, 0, memory_order_relaxed);
    atomic_store_explicit(&g_updates, 0, memory_order_relaxed);
    atomic_store_explicit(&g_turns, 0, memory_order_relaxed);
    atomic_store_explicit(&g_stick_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&g_held, 0, memory_order_relaxed);
    atomic_store_explicit(&g_count_total, 0, memory_order_relaxed);
}

/* ------------------------------------------------------------- the hooks */

/* Bloodborne 1.09's NS_SPRJ::ChrExFollowCam::Update and the instructions the step displaces,
 * at the addresses bbhost's own hooks use. The linked image keeps the eboot's layout, so the
 * guest address of a site is image + (address - 0x400000). */
#define GUEST_IMAGE_BASE 0x400000u
#define GUEST_UPDATE 0x183ac60u  /* push rbp; mov rbp, rsp; push r15..rbx; sub rsp, 0x3d8 */
#define GUEST_STORE 0x183ce54u   /* the free camera's store of pitch and yaw */
#define GUEST_RESUME 0x183ce67u  /* where the stick's step, and the store, go on */
#define GUEST_RAMP1 0x183f9bcu   /* the fast turn's two exits */
#define GUEST_RAMP2 0x183fabfu

static const uint8_t kUpdateBytes[20] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
    0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xd8, 0x03, 0x00, 0x00
};
static const uint8_t kStoreBytes[19] = {
    0xc4, 0xc1, 0x7a, 0x11, 0x85, 0x40, 0x01, 0x00, 0x00, 0xc4,
    0xc3, 0x79, 0x17, 0x8d, 0x44, 0x01, 0x00, 0x00, 0x01
};
static const uint8_t kRampBytes[2][14] = {
    {0xc4, 0xc1, 0x7a, 0x11, 0x85, 0x40, 0x01, 0x00, 0x00, 0xe9, 0x9d, 0xd4, 0xff, 0xff},
    {0xc4, 0xc1, 0x7a, 0x11, 0x85, 0x40, 0x01, 0x00, 0x00, 0xe9, 0x9a, 0xd3, 0xff, 0xff}
};

/* The camera's own turns as the character moves: the four stores the community patch "Disable
 * Camera Auto Rotation via Movement" (Imedved, Kyo) makes no-ops, each vmovss [r13+N], xmm -
 * 9 bytes. The mouse holds three of them (all but the auto turn's step, which only R3's
 * recentre reaches); a patch that made them no-ops already keeps them, with any device. */
static const uint64_t kHoldSites[4] = {0x183c6e8u, 0x183c870u, 0x183c984u, 0x183dde6u};
static const uint8_t kHoldBytes[4][9] = {
    {0xc4, 0xc1, 0x7a, 0x11, 0x85, 0x44, 0x01, 0x00, 0x00},
    {0xc4, 0xc1, 0x7a, 0x11, 0xad, 0x44, 0x01, 0x00, 0x00},
    {0xc4, 0xc1, 0x7a, 0x11, 0x8d, 0x30, 0x01, 0x00, 0x00},
    {0xc4, 0xc1, 0x7a, 0x11, 0x85, 0x94, 0x02, 0x00, 0x00}
};
static const uint8_t kNop9[9] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
static const unsigned kHeldWithMouse = 0b1101u;

/* The 1.09 image's segment fingerprint (scripts/aspect_ratio.py), the same identity the
 * aspect-ratio hooks are placed behind. */
static const char kEboot109Fingerprint[] =
    "cbe97cb9e7b830195c93e7ed139cbaaeea66302da82f1b39ca31f90a3ceccc52";

/* bbhost's register-preserving store/ramp stubs and a matching prologue stub.
 * The assembler and linker own every relocation; the two sites have different resumes.
 * All guest volatile GPRs and all 16 XMM registers survive the host callback. */
void *bb_mouse_store_callback;
void *bb_mouse_update_callback;
void *bb_mouse_store_resume;
void *bb_mouse_update_resume;
uint8_t bb_mouse_free_ran;
extern void bb_mouse_store_stub(void);
extern void bb_mouse_update_stub(void);
extern void bb_mouse_ramp_stub(void);
__asm__(
    ".text\n"
    ".globl bb_mouse_store_stub\n"
    "bb_mouse_store_stub:\n"
    "vmovss %xmm0, 0x140(%r13)\n"
    "vextractps $1, %xmm1, 0x144(%r13)\n"
    "pushfq\n"
    "push %rax\n"
    "push %rcx\n"
    "push %rdx\n"
    "push %rsi\n"
    "push %rdi\n"
    "push %r8\n"
    "push %r9\n"
    "push %r10\n"
    "push %r11\n"
    "push %rbx\n"
    "mov %rsp, %rbx\n"
    "and $-16, %rsp\n"
    "sub $256, %rsp\n"
    "vmovdqu %xmm0, 0(%rsp)\n"
    "vmovdqu %xmm1, 16(%rsp)\n"
    "vmovdqu %xmm2, 32(%rsp)\n"
    "vmovdqu %xmm3, 48(%rsp)\n"
    "vmovdqu %xmm4, 64(%rsp)\n"
    "vmovdqu %xmm5, 80(%rsp)\n"
    "vmovdqu %xmm6, 96(%rsp)\n"
    "vmovdqu %xmm7, 112(%rsp)\n"
    "vmovdqu %xmm8, 128(%rsp)\n"
    "vmovdqu %xmm9, 144(%rsp)\n"
    "vmovdqu %xmm10, 160(%rsp)\n"
    "vmovdqu %xmm11, 176(%rsp)\n"
    "vmovdqu %xmm12, 192(%rsp)\n"
    "vmovdqu %xmm13, 208(%rsp)\n"
    "vmovdqu %xmm14, 224(%rsp)\n"
    "vmovdqu %xmm15, 240(%rsp)\n"
    "mov %r13, %rdi\n"
    "lea -0x3c4(%rbp), %rsi\n"
    "vmovaps %xmm10, %xmm0\n"
    "vmovshdup %xmm10, %xmm1\n"
    "call *bb_mouse_store_callback(%rip)\n"
    "vmovdqu 0(%rsp), %xmm0\n"
    "vmovdqu 16(%rsp), %xmm1\n"
    "vmovdqu 32(%rsp), %xmm2\n"
    "vmovdqu 48(%rsp), %xmm3\n"
    "vmovdqu 64(%rsp), %xmm4\n"
    "vmovdqu 80(%rsp), %xmm5\n"
    "vmovdqu 96(%rsp), %xmm6\n"
    "vmovdqu 112(%rsp), %xmm7\n"
    "vmovdqu 128(%rsp), %xmm8\n"
    "vmovdqu 144(%rsp), %xmm9\n"
    "vmovdqu 160(%rsp), %xmm10\n"
    "vmovdqu 176(%rsp), %xmm11\n"
    "vmovdqu 192(%rsp), %xmm12\n"
    "vmovdqu 208(%rsp), %xmm13\n"
    "vmovdqu 224(%rsp), %xmm14\n"
    "vmovdqu 240(%rsp), %xmm15\n"
    "mov %rbx, %rsp\n"
    "pop %rbx\n"
    "pop %r11\n"
    "pop %r10\n"
    "pop %r9\n"
    "pop %r8\n"
    "pop %rdi\n"
    "pop %rsi\n"
    "pop %rdx\n"
    "pop %rcx\n"
    "pop %rax\n"
    "popfq\n"
    "vmovss 0x140(%r13), %xmm0\n"
    "jmp *bb_mouse_store_resume(%rip)\n"
    ".globl bb_mouse_update_stub\n"
    "bb_mouse_update_stub:\n"
    "push %rax\n"
    "push %rcx\n"
    "push %rdx\n"
    "push %rsi\n"
    "push %rdi\n"
    "push %r8\n"
    "push %r9\n"
    "push %r10\n"
    "push %r11\n"
    "push %rbx\n"
    "mov %rsp, %rbx\n"
    "and $-16, %rsp\n"
    "sub $256, %rsp\n"
    "vmovdqu %xmm0, 0(%rsp)\n"
    "vmovdqu %xmm1, 16(%rsp)\n"
    "vmovdqu %xmm2, 32(%rsp)\n"
    "vmovdqu %xmm3, 48(%rsp)\n"
    "vmovdqu %xmm4, 64(%rsp)\n"
    "vmovdqu %xmm5, 80(%rsp)\n"
    "vmovdqu %xmm6, 96(%rsp)\n"
    "vmovdqu %xmm7, 112(%rsp)\n"
    "vmovdqu %xmm8, 128(%rsp)\n"
    "vmovdqu %xmm9, 144(%rsp)\n"
    "vmovdqu %xmm10, 160(%rsp)\n"
    "vmovdqu %xmm11, 176(%rsp)\n"
    "vmovdqu %xmm12, 192(%rsp)\n"
    "vmovdqu %xmm13, 208(%rsp)\n"
    "vmovdqu %xmm14, 224(%rsp)\n"
    "vmovdqu %xmm15, 240(%rsp)\n"
    "call *bb_mouse_update_callback(%rip)\n"
    "vmovdqu 0(%rsp), %xmm0\n"
    "vmovdqu 16(%rsp), %xmm1\n"
    "vmovdqu 32(%rsp), %xmm2\n"
    "vmovdqu 48(%rsp), %xmm3\n"
    "vmovdqu 64(%rsp), %xmm4\n"
    "vmovdqu 80(%rsp), %xmm5\n"
    "vmovdqu 96(%rsp), %xmm6\n"
    "vmovdqu 112(%rsp), %xmm7\n"
    "vmovdqu 128(%rsp), %xmm8\n"
    "vmovdqu 144(%rsp), %xmm9\n"
    "vmovdqu 160(%rsp), %xmm10\n"
    "vmovdqu 176(%rsp), %xmm11\n"
    "vmovdqu 192(%rsp), %xmm12\n"
    "vmovdqu 208(%rsp), %xmm13\n"
    "vmovdqu 224(%rsp), %xmm14\n"
    "vmovdqu 240(%rsp), %xmm15\n"
    "mov %rbx, %rsp\n"
    "pop %rbx\n"
    "pop %r11\n"
    "pop %r10\n"
    "pop %r9\n"
    "pop %r8\n"
    "pop %rdi\n"
    "pop %rsi\n"
    "pop %rdx\n"
    "pop %rcx\n"
    "pop %rax\n"
    ".byte 0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0xd8,0x03,0,0\n"
    "jmp *bb_mouse_update_resume(%rip)\n"
    ".globl bb_mouse_ramp_stub\n"
    "bb_mouse_ramp_stub:\n"
    "vmovss %xmm0, 0x140(%r13)\n"
    "movb $1, bb_mouse_free_ran(%rip)\n"
    "jmp *bb_mouse_store_resume(%rip)\n"
);

static uint8_t *g_image;
static uint8_t *g_hold_code[4];
static int g_hold_known;   /* the sites' own bytes are readable and unhooked */
static int g_hold_nopped;  /* the nops are in now */
static int g_hold_fixed;   /* a patch made them noops already: leave them */
static int g_installed;
static atomic_int g_wanted;       /* input.ini says direct and the hotkey has not turned it off */
static int g_setup_failed;
static const char *g_state = "not placed yet";

static void *guest_ptr(uint64_t vaddr) {
    if (!g_image || vaddr < GUEST_IMAGE_BASE) return NULL;
    return g_image + (vaddr - GUEST_IMAGE_BASE);
}

static int protect_range(void *p, size_t len, int writable, int executable) {
    if (!p || !len) return 0;
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const uintptr_t page = info.dwPageSize ? info.dwPageSize : 4096;
    const uintptr_t start = (uintptr_t)p & ~(page - 1);
    const uintptr_t end = ((uintptr_t)p + len + page - 1) & ~(page - 1);
    DWORD old = 0;
    DWORD mode = writable ? PAGE_EXECUTE_READWRITE : (executable ? PAGE_EXECUTE_READ : PAGE_READONLY);
    if (!VirtualProtect((void *)start, (SIZE_T)(end - start), mode, &old)) return 0;
    FlushInstructionCache(GetCurrentProcess(), (void *)start, (SIZE_T)(end - start));
    return 1;
#else
    const long page = sysconf(_SC_PAGESIZE);
    const uintptr_t size = page > 0 ? (uintptr_t)page : 4096;
    const uintptr_t start = (uintptr_t)p & ~(size - 1);
    const uintptr_t end = ((uintptr_t)p + len + size - 1) & ~(size - 1);
    int mode = PROT_READ | (writable ? PROT_WRITE : 0) | (executable ? PROT_EXEC : 0);
    if (mprotect((void *)start, end - start, mode) != 0) return 0;
    return 1;
#endif
}

/* jmp [rip+0] to `dest` over the first bytes of `at`, the rest int3. */
static int redirect(void *at, size_t n, const void *dest) {
    if (n < 14u) return 0;
    if (!protect_range(at, n, 1, 1)) return 0;
    uint8_t *code = at;
    code[0] = 0xff;
    code[1] = 0x25;
    memset(code + 2, 0, 4);
    const uint64_t to = (uint64_t)(uintptr_t)dest;
    memcpy(code + 6, &to, 8);
    memset(code + 14, 0xcc, n - 14);
    if (!protect_range(at, n, 0, 1)) return 0;
    return 1;
}

/* The game's update holding auto-rotation when the mouse would: the three stores as nops, or
 * as the game's own. On the camera's thread, before the update runs. */
static void sync_hold(int hold) {
    if (!g_hold_known || g_hold_fixed || !g_installed || hold == g_hold_nopped) return;
    for (int i = 0; i < 4; ++i) {
        if (!g_hold_code[i] || !((kHeldWithMouse >> i) & 1u)) continue;
        if (!protect_range(g_hold_code[i], 9, 1, 1)) continue;
        memcpy(g_hold_code[i], hold ? kNop9 : kHoldBytes[i], 9);
        protect_range(g_hold_code[i], 9, 0, 1);
    }
    g_hold_nopped = hold;
}

/* The free camera of the game's update, after the stick has turned it: DS3's mouse step. */
static BB_GUEST_ABI void store_hook(uint8_t *cam, uint8_t *input, float stick_pitch, float stick_yaw) {
    if (!cam) return;
    mouse_camera_step_free_ran();
    mouse_camera_step_turn_camera(cam, input, stick_pitch, stick_yaw);
}

/* Every camera update, first: this update's counts, and the game's code holding the camera's
 * own turns or not. */
static BB_GUEST_ABI void update_hook(void) {
    mouse_camera_step_begin_update();
    sync_hold(mouse_camera_step_held() && mouse_camera_step_active());
}

static int guard_allows(void) {
    /* run.py publishes the image's segment fingerprint for every launch (BB_INPUT_EBOOT_SEGMENTS);
     * the aspect-ratio and mouse-camera names are older and still read as fallbacks. */
    const char *sig = getenv("BB_INPUT_EBOOT_SEGMENTS");
    if (!sig || !*sig) sig = getenv("BB_MOUSE_CAMERA_EBOOT_SEGMENTS");
    if (!sig || !*sig) sig = getenv("BB_ASPECT_EBOOT_SEGMENTS");
    if (!sig || !*sig) return 0;
    return strcmp(sig, kEboot109Fingerprint) == 0;
}

void mouse_camera_bind_image(void *image, uint64_t size) {
    if (g_installed || g_setup_failed) return;
    if (!image || size <= GUEST_RAMP2 - GUEST_IMAGE_BASE + 14) {
        g_setup_failed = 1;
        g_state = "the image is too small for the follow camera";
        printf("Mouse camera: refused, %s; the mouse turns the camera through the stick\n", g_state);
        return;
    }
    g_image = image;
    if (!guard_allows()) {
        /* Not the verified 1.09 image: leave the game's own code alone. */
        g_setup_failed = 1;
        g_state = "the game is not the verified 1.09 build";
        printf("Mouse camera: %s (input.ini's mouse_camera = direct needs it); the mouse turns "
               "the camera through the stick\n", g_state);
        return;
    }
    uint8_t *update = guest_ptr(GUEST_UPDATE);
    uint8_t *store = guest_ptr(GUEST_STORE);
    uint8_t *ramp[2] = {guest_ptr(GUEST_RAMP1), guest_ptr(GUEST_RAMP2)};
    if (memcmp(update, kUpdateBytes, sizeof(kUpdateBytes)) != 0) {
        g_setup_failed = 1;
        g_state = "the follow camera's update is not the 1.09 code";
        printf("Mouse camera: refused, %s; the mouse turns the camera through the stick\n", g_state);
        return;
    }
    if (memcmp(store, kStoreBytes, sizeof(kStoreBytes)) != 0) {
        g_setup_failed = 1;
        g_state = "the follow camera's step is not the 1.09 code";
        printf("Mouse camera: refused, %s; the mouse turns the camera through the stick\n", g_state);
        return;
    }
    for (int i = 0; i < 4; ++i) {
        uint8_t *code = guest_ptr(kHoldSites[i]);
        if (!code) continue;
        if (memcmp(code, kHoldBytes[i], 9) == 0) g_hold_code[i] = code;
        else if (memcmp(code, kNop9, 9) == 0) g_hold_fixed = 1;
    }
    g_hold_known = g_hold_code[0] || g_hold_code[2] || g_hold_code[3];

    /* Validate all four sites before changing any instruction. */
    for (int i = 0; i < 2; ++i) {
        if (memcmp(ramp[i], kRampBytes[i], 14) != 0) {
            g_setup_failed = 1;
            g_state = "a fast-turn exit was changed by another patch";
            printf("Mouse camera: refused, %s; leaving the guest code unchanged\n", g_state);
            return;
        }
    }
    bb_mouse_store_callback = (void *)store_hook;
    bb_mouse_update_callback = (void *)update_hook;
    bb_mouse_store_resume = guest_ptr(GUEST_RESUME);
    bb_mouse_update_resume = update + sizeof(kUpdateBytes);
    /* Code pages are mapped before guest execution; make all sites writable first. */
    if (!protect_range(update, sizeof(kUpdateBytes), 1, 1) ||
        !protect_range(store, sizeof(kStoreBytes), 1, 1) ||
        !protect_range(ramp[0], 14, 1, 1) || !protect_range(ramp[1], 14, 1, 1)) {
        g_setup_failed = 1;
        g_state = "camera hook pages could not be made writable";
        return;
    }
    if (!redirect(store, sizeof(kStoreBytes), bb_mouse_store_stub) ||
        !redirect(ramp[0], 14, bb_mouse_ramp_stub) ||
        !redirect(ramp[1], 14, bb_mouse_ramp_stub) ||
        !redirect(update, sizeof(kUpdateBytes), bb_mouse_update_stub)) {
        /* Restore all originals before allowing the guest to run. */
        void *sites[] = {store, ramp[0], ramp[1], update};
        const void *original[] = {kStoreBytes, kRampBytes[0], kRampBytes[1], kUpdateBytes};
        const size_t lengths[] = {sizeof(kStoreBytes), 14, 14, sizeof(kUpdateBytes)};
        for (int i = 0; i < 4; ++i) {
            if (protect_range(sites[i], lengths[i], 1, 1)) {
                memcpy(sites[i], original[i], lengths[i]);
                protect_range(sites[i], lengths[i], 0, 1);
            }
        }
        g_setup_failed = 1;
        g_state = "camera hooks rolled back after a write failure";
        return;
    }
    g_installed = 1;
    g_state = "placed";
    if (g_hold_fixed)
        printf("Mouse camera: a patch holds the camera's auto-rotation already, with any device\n");
    printf("Mouse camera: direct - %.3f degrees a count at sensitivity 5, after the stick and "
           "only without it; a flick switches the lock-on target\n",
           mouse_camera_degrees_per_count(5));
}

/* ------------------------------------------------------------ the pad read */

/* The lock-on flick, decided on the pad read (DS3's own thresholds, as rates so Bloodborne's
 * 30 fps asks for the same movement). */
#define FLICK_SWITCH_COUNTS_PER_SECOND (50.0f * 60.0f)
#define FLICK_REARM_COUNTS_PER_SECOND (25.0f * 60.0f)
#define FLICK_COOLDOWN_SECONDS 0.5f
#define FLICK_HOLD_MS 80

static double now_seconds(void) {
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

static double g_last_flick_read;
static double g_flick_cooldown;
static double g_flick_until;
static float g_flick_x, g_flick_y;

int mouse_camera_flick(float *x, float *y) {
    if (!mouse_camera_step_active()) return 0;
    const double now = now_seconds();
    double dt = g_last_flick_read > 0.0 ? now - g_last_flick_read : 0.0;
    if (dt > 0.25) dt = 0.25; /* a load or a pause is not a fast mouse */
    g_last_flick_read = now;
    float dx = 0.0f, dy = 0.0f;
    uint32_t updates = 0;
    mouse_camera_step_take_flick_counts(&dx, &dy, &updates);
    if (updates != 0 && dt > 0.0) {
        const float magnitude = sqrtf(dx * dx + dy * dy);
        const float rate = (float)(magnitude / dt);
        if (rate < FLICK_REARM_COUNTS_PER_SECOND) {
            g_flick_cooldown = 0.0;
        } else {
            g_flick_cooldown = g_flick_cooldown - dt < 0.0 ? 0.0 : g_flick_cooldown - dt;
        }
        if (g_flick_cooldown <= 0.0 && rate > FLICK_SWITCH_COUNTS_PER_SECOND && magnitude > 0.0f) {
            g_flick_x = dx / magnitude;
            g_flick_y = dy / magnitude;
            g_flick_until = now + FLICK_HOLD_MS / 1000.0;
            g_flick_cooldown = FLICK_COOLDOWN_SECONDS;
            mouse_camera_step_report_flick();
            if (mouse_camera_trace_on()) {
                printf("Mouse camera: a flick of %.0f counts (%.0f a second) switches the lock-on "
                       "target, toward %+.2f,%+.2f\n", magnitude, rate, g_flick_x, g_flick_y);
            }
        }
    }
    if (now >= g_flick_until) return 0;
    *x = g_flick_x;
    *y = g_flick_y;
    return 1;
}

/* ------------------------------------------------------------- the state */

int mouse_camera_installed(void) { return g_installed; }

int mouse_camera_active(void) {
    return g_installed && atomic_load_explicit(&g_wanted, memory_order_acquire);
}

void mouse_camera_set_enabled(int enabled) {
    const int wanted = enabled ? 1 : 0;
    if (wanted == atomic_exchange_explicit(&g_wanted, wanted, memory_order_acq_rel)) return;
    mouse_camera_step_set_active(g_installed && wanted);
    static int logged;
    if (!g_setup_failed && (logged < 8 || mouse_camera_trace_on())) {
        ++logged;
        printf("Mouse camera: %s (%s)\n", g_wanted ? "on, the mouse turns the camera" : "off, the camera is the pad's",
               g_state);
    }
}

void mouse_camera_motion(float dx, float dy) {
    if (!mouse_camera_active()) return;
    mouse_camera_step_add_counts(dx, dy);
}

void mouse_camera_publish(int sensitivity, float sensitivity_multiplier, float scale,
                          int invert_x, int invert_y, int hold_auto_rotation) {
    float multiplier = sensitivity_multiplier;
    if (!(multiplier > 0.0f) || multiplier > 100.0f) multiplier = 1.0f;
    const float per_count = mouse_camera_degrees_per_count_scaled(sensitivity, multiplier * scale);
    uint32_t flags = 0;
    if (invert_x) flags |= MOUSE_CAMERA_INVERT_X;
    if (invert_y) flags |= MOUSE_CAMERA_INVERT_Y;
    if (hold_auto_rotation && mouse_camera_active()) flags |= MOUSE_CAMERA_HOLD_AUTO_ROTATION;
    mouse_camera_step_publish(per_count, flags);
}

/* -------------------------------------------------------------- reporting */

int mouse_camera_trace_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *env = getenv("BB_MOUSE_TRACE");
        on = env && env[0] == '1' ? 1 : 0;
    }
    return on;
}

static double g_trace_last;
static unsigned long long g_trace_samples, g_trace_motion, g_trace_injected;
static double g_trace_dx, g_trace_dy;
static char g_trace_route[32];

void mouse_camera_trace(const char *route, float dx, float dy, int stick_x, int stick_y) {
    if (!mouse_camera_trace_on()) return;
    g_trace_samples++;
    if (dx != 0.0f || dy != 0.0f) {
        g_trace_motion++;
        g_trace_dx += dx;
        g_trace_dy += dy;
    }
    if (stick_x || stick_y) g_trace_injected++;
    if (route) {
        snprintf(g_trace_route, sizeof(g_trace_route), "%s", route);
    }
    const double now = now_seconds();
    if (g_trace_last <= 0.0) {
        g_trace_last = now;
        return;
    }
    if (now - g_trace_last < 1.0) return;
    MouseCameraStepStats stats;
    mouse_camera_step_take_stats(&stats);
    printf("Mouse trace: %.1fs %s: %llu samples, %llu with motion (%+.1f,%+.1f counts), "
           "%llu to the stick (%d,%d); updates %llu, turned %llu, stick had it %llu, held %llu, "
           "counts %llu, flicks %llu\n",
           now - g_trace_last, g_trace_route[0] ? g_trace_route : "?", g_trace_samples, g_trace_motion,
           g_trace_dx, g_trace_dy, g_trace_injected, stick_x, stick_y, (unsigned long long)stats.updates,
           (unsigned long long)stats.turns, (unsigned long long)stats.stick_frames,
           (unsigned long long)stats.held, (unsigned long long)stats.counts, (unsigned long long)stats.flicks);
    fflush(stdout);
    g_trace_last = now;
    g_trace_samples = g_trace_motion = g_trace_injected = 0;
    g_trace_dx = g_trace_dy = 0.0;
}

void mouse_camera_report(void) {
    if (!g_installed && !g_setup_failed) return;
    MouseCameraStepStats stats;
    mouse_camera_step_take_stats(&stats);
    printf("Mouse camera (%s): %llu camera updates, %llu turned by the mouse, %llu where the "
           "stick had the camera, %llu lock-on flicks, auto-rotation held in %llu\n",
           g_state, (unsigned long long)stats.updates, (unsigned long long)stats.turns,
           (unsigned long long)stats.stick_frames, (unsigned long long)stats.flicks,
           (unsigned long long)stats.held);
    fflush(stdout);
}
