// bbport: SDL3 window for the Vulkan swapchain (X11 or Wayland).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <cstdio>
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "common/assert.h"
#include "common/logging/log.h"
#include "sdl_window.h"
#include "runtime_mouse_camera.h"
#include "menu_pointer.h"
#include "bbport_overlay.h"
#include "bbport_prompts.h"

namespace Frontend {

/// The buttons a controller has, for its prompts: SDL's type, by vendor and product.
static BbPrompts::Device PadDevice(SDL_JoystickID id) {
    switch (SDL_GetGamepadTypeForID(id)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return BbPrompts::PlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return BbPrompts::Switch;
    default:
        return BbPrompts::Xbox; // Xbox, and the many controllers that copy its buttons
    }
}

namespace {
// MOU-007: each wheel step keeps its direction's bit set for 33 ms (shadPS4's pulse length,
// ported so a config's mouse_movement_params tuning carries over); consecutive steps extend
// it rather than stacking. One expiry timestamp per direction, window-thread only.
constexpr uint32_t kWheelPulseMs = 33;
std::array<uint64_t, 4> g_wheel_expiry_ms{}; // index: WHEEL_UP=0, DOWN=1, LEFT=2, RIGHT=3
} // namespace

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    // Gamepads are sampled by runtime_pad.c; their events are pumped here with the window's.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        UNREACHABLE_MSG("Failed to initialize SDL video: {}", SDL_GetError());
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width_);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height_);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    const char* fullscreen = std::getenv("BB_FULLSCREEN");
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, fullscreen && fullscreen[0] == '1');
    base_title = title;
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    ASSERT_MSG(window, "Failed to create window: {}", SDL_GetError());

    const char* driver = SDL_GetCurrentVideoDriver();
    const SDL_PropertiesID wp = SDL_GetWindowProperties(window);
#ifdef _WIN32
    if (driver && !std::strcmp(driver, "windows")) {
        window_info.type = WindowSystemType::Windows;
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else
#endif
    if (driver && !std::strcmp(driver, "x11")) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(SDL_GetNumberProperty(wp, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    } else if (driver && !std::strcmp(driver, "wayland")) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface = SDL_GetPointerProperty(wp, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        UNREACHABLE_MSG("Unsupported SDL video driver {}", driver ? driver : "(none)");
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    width = w;
    height = h;
    LOG_INFO(Frontend, "Window {}x{} on {}", w, h, driver);
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0");
}

WindowSDL::~WindowSDL() {
    mouse_camera_set_enabled(0);
    SDL_DestroyWindow(window);
}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    std::scoped_lock lock{text_mutex};
    text = initial;
    text_prompt = prompt;
    text_state = 0;
    text_requested = true;
}

int WindowSDL::PollTextInput(std::string& out) {
    std::scoped_lock lock{text_mutex};
    out = text;
    return text_state;
}

void WindowSDL::UpdateTextTitle() {
    const std::string title = text_active ? base_title + " \u2014 " + text_prompt + ": " + text + "_  (Enter = OK, Esc = cancel)"
                                          : base_title;
    SDL_SetWindowTitle(window, title.c_str());
    BbOverlay::SetTextEntry(text_active, text_prompt, text);
}

bool WindowSDL::HasInputFocus() const {
#ifdef _WIN32
    // SDL key state is shared across windows. Check OS ownership too, so another
    // app's input cannot reach the guest while a focus event is still pending.
    return window_focused.load(std::memory_order_relaxed) &&
        GetForegroundWindow() == static_cast<HWND>(window_info.render_surface);
#else
    return window_focused.load(std::memory_order_relaxed);
#endif
}

bool WindowSDL::PollEvents() {
    {
        std::scoped_lock lock{text_mutex};
        if (text_requested) { // SDL text input must be toggled from the window thread
            text_requested = false;
            text_active = true;
            SDL_StartTextInput(window);
            UpdateTextTitle();
        }
    }
    if (!text_active) {
        BbOverlay::UpdateTextInput(window);
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Button prompts follow the device that was used last: a key, a click, the wheel or a
        // real mouse movement, or a controller button or a stick or trigger pushed past half.
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
            if (!event.key.repeat) BbPrompts::OnInput(BbPrompts::Keyboard);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_WHEEL:
            BbPrompts::OnInput(BbPrompts::Keyboard);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (std::abs(event.motion.xrel) + std::abs(event.motion.yrel) >= 3.0f) {
                BbPrompts::OnInput(BbPrompts::Keyboard);
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            BbPrompts::OnInput(PadDevice(event.gbutton.which));
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            // The game starts with the controller's prompts.
            std::printf("Prompts: controller %s: %s\n", SDL_GetGamepadNameForID(event.gdevice.which),
                        SDL_GetGamepadStringForType(SDL_GetGamepadTypeForID(event.gdevice.which)));
            BbPrompts::OnControllerConnected(PadDevice(event.gdevice.which));
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (std::abs(int(event.gaxis.value)) > 16000) BbPrompts::OnInput(PadDevice(event.gaxis.which));
            break;
        default:
            break;
        }
        // HOT-003: the toggle/reload hotkeys work regardless of the menu or text dialog (F8
        // must reload input.ini even with the menu open), so they are handled before anything
        // else gets a chance to consume the key event. Held-down repeats are ignored the same
        // way the menu's own Insert/Escape toggle already is (!event.key.repeat).
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            const int32_t scancode = static_cast<int32_t>(event.key.scancode);
            if (scancode == toggle_scancode.load(std::memory_order_relaxed) &&
                mouse_mode_available.load(std::memory_order_relaxed)) {
                mouse_mode_on.store(!mouse_mode_on.load(std::memory_order_relaxed), std::memory_order_relaxed);
                continue;
            }
            if (scancode == reload_scancode.load(std::memory_order_relaxed)) {
                reload_requested.store(true, std::memory_order_relaxed);
                continue;
            }
        }
        if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
            window_focused = true;
        } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            window_focused = false;
        }
        // Select the last actively used input device. Analog drift never owns the pointer.
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
            (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && std::abs(int(event.gaxis.value)) > 9000)) {
            menu_pointer_device(1);
        } else if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                   event.type == SDL_EVENT_MOUSE_WHEEL ||
                   (event.type == SDL_EVENT_MOUSE_MOTION && (event.motion.xrel || event.motion.yrel))) {
            menu_pointer_device(0);
        }
        if (!BbOverlay::CapturesInput() && !text_active && HasInputFocus()) {
            if (mouse_captured_last) {
                if (event.type == SDL_EVENT_MOUSE_MOTION) {
                    if (mouse_camera_active()) {
                        mouse_camera_motion(event.motion.xrel, event.motion.yrel);
                    } else {
                        std::scoped_lock lock{mouse_mutex};
                        mouse.dx += event.motion.xrel;
                        mouse.dy += event.motion.yrel;
                    }
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    std::scoped_lock lock{mouse_mutex};
                    const uint32_t mask = SDL_BUTTON_MASK(event.button.button);
                    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) { mouse.buttons |= mask; mouse_pressed |= mask; }
                    else mouse.buttons &= ~mask;
                } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                    const uint64_t expiry = SDL_GetTicks() + kWheelPulseMs;
                    if (event.wheel.y > 0) g_wheel_expiry_ms[0] = expiry;
                    else if (event.wheel.y < 0) g_wheel_expiry_ms[1] = expiry;
                    if (event.wheel.x > 0) g_wheel_expiry_ms[3] = expiry;
                    else if (event.wheel.x < 0) g_wheel_expiry_ms[2] = expiry;
                }
            } else if (menu_pointer_in_menu()) {
                // Update the point before recording a click in this same event batch.
                int cw = 0, ch = 0, ow = 1920, oh = 1080;
                SDL_GetWindowSize(window, &cw, &ch);
                if (const char *resolution = std::getenv("BB_OUTPUT_RES"))
                    std::sscanf(resolution, "%dx%d", &ow, &oh);
                float x = 0, y = 0; SDL_GetMouseState(&x, &y);
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) { x = event.button.x; y = event.button.y; }
                menu_pointer_position(x, y, cw, ch, ow, oh, 1, 0, 0);
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT)
                    menu_pointer_click();
                else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT)
                    menu_pointer_back();
                else if (event.type == SDL_EVENT_MOUSE_WHEEL)
                    menu_pointer_scroll(event.wheel.x, event.wheel.y);
            }
        }
        if (text_active && (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN)) {
            std::scoped_lock lock{text_mutex};
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                text += event.text.text;
            } else if (event.key.key == SDLK_BACKSPACE && !text.empty()) {
                size_t cut = text.size() - 1; // drop one UTF-8 code point
                while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
                text.erase(cut);
            } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER || event.key.key == SDLK_ESCAPE) {
                text_state = event.key.key == SDLK_ESCAPE ? 2 : 1;
                text_active = false;
                SDL_StopTextInput(window);
            }
            UpdateTextTitle();
            continue;
        }
        // The controller finishes the text dialog too: Cross (A) accepts, Circle (B) cancels.
        if (text_active && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
            (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
             event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)) {
            std::scoped_lock lock{text_mutex};
            text_state = event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : 2;
            text_active = false;
            SDL_StopTextInput(window);
            UpdateTextTitle();
            continue;
        }
        if (BbOverlay::HandleEvent(event)) {
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            width = w;
            height = h;
            break;
        }
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    // MOU-004: re-evaluated every PollEvents (not only on the events that could change it),
    // because the menu can also close from the render thread (bbport_overlay.cpp's "Close"
    // button sets menu_open there, not through an SDL event here).
    UpdateMouseCapture();
    BbPrompts::Apply();
    UpdateMouseCamera();
    return is_open;
}

void WindowSDL::UpdateMouseCamera() {
    const bool active = direct_camera.load(std::memory_order_relaxed) && mouse_captured_last;
    mouse_camera_set_enabled(active);
    int cw = 0, ch = 0, ow = 1920, oh = 1080;
    SDL_GetWindowSize(window, &cw, &ch);
    if (const char *resolution = std::getenv("BB_OUTPUT_RES"))
        std::sscanf(resolution, "%dx%d", &ow, &oh);
    float x = 0, y = 0; SDL_GetMouseState(&x, &y);
    menu_pointer_position(x, y, cw, ch, ow, oh, HasInputFocus(), mouse_captured_last,
                           text_active || BbOverlay::CapturesInput());
}

bool WindowSDL::WantsMouseCapture() const {
    // MOU-003: all of mouse mode on, window focused, menu closed, text dialog inactive.
    // BbOverlay::CapturesInput() already covers both the menu and the text entry box.
    return mouse_mode_on.load(std::memory_order_relaxed) && HasInputFocus() && !BbOverlay::CapturesInput() && !menu_pointer_in_menu();
}

void WindowSDL::UpdateMouseCapture() {
    const bool wants = WantsMouseCapture();
    if (wants == mouse_captured_last) {
        return;
    }
    mouse_camera_set_enabled(0); // Drop old-mode counts before switching ownership.
    mouse_captured_last = wants;
    SDL_SetWindowRelativeMouseMode(window, wants);
    // Enabling relative mode hides the cursor by itself; disabling it does not document giving
    // it back, so that side is made explicit here (SDL_ShowCursor is a no-op if already shown).
    if (!wants) {
        SDL_ShowCursor();
    }
    {
        // MOU-005/MOU-007: leaving capture drops whatever motion/buttons/wheel accumulated so
        // far -- a button still held when capture ends must never reach the pad as a press.
        std::scoped_lock lock{mouse_mutex};
        mouse.dx = mouse.dy = 0.0f;
        mouse.buttons = 0;
        mouse_pressed = 0;
        mouse.wheel = 0;
        mouse.captured = wants;
    }
    if (!wants) {
        g_wheel_expiry_ms.fill(0);
    }
}

void WindowSDL::TakeMouseInput(MouseInput& out) {
    std::scoped_lock lock{mouse_mutex};
    const uint64_t now = SDL_GetTicks();
    mouse.wheel = 0;
    for (size_t i = 0; i < g_wheel_expiry_ms.size(); ++i) {
        if (g_wheel_expiry_ms[i] > now) mouse.wheel |= (1u << i);
    }
    out = mouse;
    out.buttons |= mouse_pressed;
    mouse_pressed = 0;
    // Motion is drained (MOU-008: a delta is reported once, to the next sample only); held
    // buttons are not -- a button still down stays down across calls until its SDL_EVENT_MOUSE_
    // BUTTON_UP, same as SDL_GetMouseState would report. UpdateMouseCapture() is what clears
    // buttons/wheel on the capture->uncaptured transition (MOU-005/MOU-007), not this function.
    mouse.dx = mouse.dy = 0.0f;
}

void WindowSDL::ConfigureInput(bool mouse_mode_available_, int32_t toggle_scancode_, int32_t reload_scancode_) {
    mouse_mode_available.store(mouse_mode_available_, std::memory_order_relaxed);
    toggle_scancode.store(toggle_scancode_, std::memory_order_relaxed);
    reload_scancode.store(reload_scancode_, std::memory_order_relaxed);
    if (!mouse_mode_available_) {
        // MOU-001: without mouse_to_joystick in input.ini, the mode does not exist; turning it
        // off here also covers an F8 reload that removed the line while it was on.
        mouse_mode_on.store(false, std::memory_order_relaxed);
    } else if (!input_configured_once.exchange(true, std::memory_order_relaxed)) {
        // MOU-002: "mode starts on" applies only to the very first load (game startup). A
        // later F8 reload that keeps mouse_to_joystick present must not re-enable a mode the
        // player turned off with the hotkey in between.
        mouse_mode_on.store(true, std::memory_order_relaxed);
    }
}

int WindowSDL::TakeInputReloadRequested() {
    return reload_requested.exchange(false, std::memory_order_relaxed) ? 1 : 0;
}

} // namespace Frontend
