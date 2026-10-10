// Star Wars Episode I: Racer - N64 Static Recompilation
// Main entry point
//
// "Now THIS is podracing!" - Anakin Skywalker

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <atomic>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#endif

#include <SDL.h>
#include <SDL_syswm.h>

#if defined(__linux__)
// SDL_syswm.h pulls in the X11 headers, whose macros (None, Status, Success,
// True/False, Always, ...) collide with ultramodern's enum class members.
#undef None
#undef Status
#undef Success
#undef LockMask
#undef Always
#undef True
#undef False
#undef Bool
#undef KeyPress
#undef KeyRelease
#undef FocusIn
#undef FocusOut
#undef Expose
#undef CursorShape
#undef Unsorted
#undef Convex
#undef Complex
#undef GrayScale
#undef FontChange
#undef Screen
#endif

#if !defined(_WIN32)
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#endif

#include "recomp.h"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/rsp.hpp"
#include "librecomp/addresses.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/ultra64.h"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/error_handling.hpp"
#include "ultramodern/events.hpp"
#include "ultramodern/input.hpp"
#include "ultramodern/threads.hpp"

// =============================================================================
// External declarations
// =============================================================================

// From section_table.cpp
extern SectionTableEntry section_table[];
extern const size_t num_sections;

// From rt64_render_context.cpp
extern std::unique_ptr<ultramodern::renderer::RendererContext> create_swe1r_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

// The recompiled entrypoint function
extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);

// From audio.cpp
extern void swe1r_queue_samples(int16_t* samples, size_t num_samples);
extern size_t swe1r_get_frames_remaining();
extern void swe1r_set_frequency(uint32_t freq);

// RDRAM base — captured in on_game_init so the crash handler can translate
// native fault addresses back into N64 virtual addresses.
static uint8_t* g_rdram_base = nullptr;

// The game's __osSiRawStartDma moves the 64-byte PIF RAM block over COP0/MMIO,
// so the adapter owns the PIF: a write latches the guest's joybus command
// block, a read executes it against the host pad on channel 0 and returns the
// block with responses filled in. Other channels report no device.
struct HostPad {
    uint16_t buttons;
    int8_t stick_x;
    int8_t stick_y;
};

static std::atomic<uint32_t> g_host_pad{ 0 };

static HostPad read_host_pad() {
    const uint32_t packed = g_host_pad.load(std::memory_order_relaxed);
    return { (uint16_t)packed, (int8_t)(packed >> 16), (int8_t)(packed >> 24) };
}

static uint32_t pack_host_pad(const HostPad& pad) {
    return pad.buttons | ((uint32_t)(uint8_t)pad.stick_x << 16) | ((uint32_t)(uint8_t)pad.stick_y << 24);
}

constexpr uint32_t pif_ram_size = 64;
constexpr uint8_t joybus_no_response = 0x80;

static uint8_t g_pif_ram[pif_ram_size];

static bool joybus_controller(const uint8_t* send, uint8_t* recv, uint32_t rx) {
    switch (send[0]) {
    case 0x00:
    case 0xFF: {
        const uint8_t status[] = { 0x05, 0x00, 0x00 };
        memcpy(recv, status, std::min<uint32_t>(rx, sizeof(status)));
        return true;
    }
    case 0x01: {
        const HostPad pad = read_host_pad();
        const uint8_t state[] = { (uint8_t)(pad.buttons >> 8), (uint8_t)pad.buttons, (uint8_t)pad.stick_x, (uint8_t)pad.stick_y };
        memcpy(recv, state, std::min<uint32_t>(rx, sizeof(state)));
        static uint32_t last_reported = 0xFFFFFFFFu;
        if (pack_host_pad(pad) != last_reported) {
            last_reported = pack_host_pad(pad);
            printf("[si] guest read pad buttons=0x%04X stick=%d,%d\n", pad.buttons, pad.stick_x, pad.stick_y);
        }
        return true;
    }
    default:
        printf("[si] controller command 0x%02X unsupported\n", send[0]);
        return false;
    }
}

static void execute_pif_ram(uint8_t* rdram, gpr dram) {
    uint32_t channel = 0;
    uint32_t i = 0;
    while (i < pif_ram_size - 1) {
        const uint8_t tx_byte = g_pif_ram[i];
        if (tx_byte == 0xFE) break;
        if (tx_byte == 0xFF) { i++; continue; }
        if (tx_byte == 0x00 || tx_byte == 0xFD) { channel++; i++; continue; }
        const uint32_t tx = tx_byte & 0x3F;
        const uint32_t rx = g_pif_ram[i + 1] & 0x3F;
        const uint32_t send = i + 2;
        const uint32_t recv = send + tx;
        if (tx == 0 || recv + rx > pif_ram_size - 1) break;
        const bool answered = channel == 0 && joybus_controller(&g_pif_ram[send], &g_pif_ram[recv], rx);
        if (!answered) g_pif_ram[i + 1] |= joybus_no_response;
        i = recv + rx;
        channel++;
    }
    g_pif_ram[pif_ram_size - 1] = 0;
    for (uint32_t b = 0; b < pif_ram_size; b++) MEM_B(b, dram) = (int8_t)g_pif_ram[b];
}

extern "C" void __osSiRawStartDma_recomp(uint8_t* rdram, recomp_context* ctx) {
    const gpr dram = ctx->r5;
    if ((int32_t)ctx->r4 == 1) {
        for (uint32_t b = 0; b < pif_ram_size; b++) g_pif_ram[b] = MEM_BU(b, dram);
    } else {
        execute_pif_ram(rdram, dram);
    }
    ultramodern::send_si_message();
    ctx->r2 = 0;
}

// =============================================================================
// Native sprintf (game func_8008A6B4, misidentified as "dma_read_rom")
// =============================================================================
// The game's sprintf wrapper funnels into a vformatter (func_80093180) whose
// conversion dispatcher (func_80092B10) executes an indirect jump through a
// ROM jump table that static recompilation cannot resolve; the whole printf
// cluster (dispatcher, itoa, ftoa) is stubbed, which left the emit path
// dereferencing an uninitialised segment descriptor pointer and crashing in
// bcopy the first time the menu code formatted a string.
//
// This reimplements the wrapper's contract natively:
//   a0 = dst, a1 = fmt, varargs in a2/a3 + caller stack (o32: 16-byte home
//   area at caller sp+0x00..0x0C, stack args at sp+0x10 and up). Returns the
//   number of chars written; dst is NUL-terminated.
//
// Arg fetch replicates the original dispatcher's vararg walk: 8-byte
// alignment for 'L' and doubles, 32-bit fetch otherwise; 'l' on both signed
// and unsigned integer conversions fetches 32 bits and sign-extends (the
// original sign-extends even for %lu — a quirk preserved here). Host snprintf
// applies flags/width/precision per directive; its semantics match the
// original formatter for the C89 set the game uses. %p prints the 32-bit
// value (the original prints the sign-extension word, which is useless).
extern "C" void swe1r_sprintf(uint8_t* rdram, recomp_context* ctx) {
    // MEM_* macros expect sign-extended 64-bit guest addresses
    // (0xFFFFFFFF80xxxxxx), as kept in ctx registers — never truncate to 32.
    const uint64_t dst = ctx->r4;
    const uint64_t fmt = ctx->r5;
    uint32_t count = 0;

    int argn = 0; // vararg index in 32-bit units; 0=a2, 1=a3, 2+=caller stack
    auto fetch32 = [&]() -> uint32_t {
        uint32_t v;
        if (argn == 0) v = (uint32_t)ctx->r6;
        else if (argn == 1) v = (uint32_t)ctx->r7;
        else v = (uint32_t)MEM_W(0x10 + 4 * (argn - 2), ctx->r29);
        argn += 1;
        return v;
    };
    auto fetch64 = [&]() -> uint64_t {
        if (argn & 1) argn++; // 8-byte alignment, as the original
        uint64_t hi = fetch32();
        uint64_t lo = fetch32();
        return (hi << 32) | lo;
    };
    auto out_byte = [&](uint8_t b) { MEM_B(count, dst) = (int8_t)b; count++; };

    for (uint32_t i = 0;; i++) {
        uint8_t ch = MEM_BU(i, fmt);
        if (ch == 0) break;
        if (ch != '%') { out_byte(ch); continue; }
        i++;

        // Collect the directive for host snprintf: flags, width, precision.
        char hfmt[40];
        int hf = 0;
        hfmt[hf++] = '%';
        uint8_t c;
        while ((c = MEM_BU(i, fmt)) == '-' || c == '+' || c == ' ' || c == '0' || c == '#') {
            hfmt[hf++] = (char)c; i++;
        }
        while ((c = MEM_BU(i, fmt)) >= '0' && c <= '9') { hfmt[hf++] = (char)c; i++; }
        if (c == '.') {
            hfmt[hf++] = '.'; i++;
            while ((c = MEM_BU(i, fmt)) >= '0' && c <= '9') { hfmt[hf++] = (char)c; i++; }
        }
        int lenmod = 0;
        if (c == 'l' || c == 'L' || c == 'h') { lenmod = c; i++; c = MEM_BU(i, fmt); }

        char tmp[512];
        int n = 0;
        switch (c) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            // Always host "ll"; truncation/length semantics applied in C++ so
            // the host vararg type always matches the format.
            uint64_t val;
            bool sign = (c == 'd' || c == 'i');
            if (lenmod == 'L') val = fetch64();
            else if (lenmod == 'l')
                // Original fetches 32 bits and sign-extends (even unsigned).
                val = (uint64_t)(int64_t)(int32_t)fetch32();
            else if (lenmod == 'h') {
                uint32_t v = fetch32();
                val = sign ? (uint64_t)(int64_t)(int16_t)v : (uint64_t)(v & 0xFFFF);
            } else {
                uint32_t v = fetch32();
                val = sign ? (uint64_t)(int64_t)(int32_t)v : (uint64_t)v;
            }
            snprintf(hfmt + hf, sizeof(hfmt) - hf, "ll%c", c);
            if (sign)
                n = snprintf(tmp, sizeof(tmp), hfmt, (long long)val);
            else
                n = snprintf(tmp, sizeof(tmp), hfmt, (unsigned long long)val);
            break;
        }
        case 'f': case 'e': case 'E': case 'g': case 'G': {
            uint64_t bits = fetch64();
            double d;
            memcpy(&d, &bits, sizeof(d));
            snprintf(hfmt + hf, sizeof(hfmt) - hf, "%c", c);
            n = snprintf(tmp, sizeof(tmp), hfmt, d);
            break;
        }
        case 'c': {
            uint32_t v = fetch32();
            snprintf(hfmt + hf, sizeof(hfmt) - hf, "c");
            n = snprintf(tmp, sizeof(tmp), hfmt, (int)v);
            break;
        }
        case 's': {
            uint64_t p = (uint64_t)(int64_t)(int32_t)fetch32();
            char sbuf[1024];
            int sl = 0;
            if (p != 0) { // original would fault on NULL; degrade to ""
                uint8_t sb;
                while (sl < (int)sizeof(sbuf) - 1 && (sb = MEM_BU(sl, p)) != 0)
                    sbuf[sl++] = (char)sb;
            }
            sbuf[sl] = 0;
            snprintf(hfmt + hf, sizeof(hfmt) - hf, "s");
            n = snprintf(tmp, sizeof(tmp), hfmt, sbuf);
            break;
        }
        case 'n': {
            uint64_t p = (uint64_t)(int64_t)(int32_t)fetch32();
            if (lenmod == 'h') MEM_H(0, p) = (int16_t)count;
            else if (lenmod == 'l' || lenmod == 'L') {
                MEM_W(0, p) = (int32_t)count;
                MEM_W(4, p) = 0;
            } else MEM_W(0, p) = (int32_t)count;
            break;
        }
        case 'p': {
            uint32_t v = fetch32();
            n = snprintf(tmp, sizeof(tmp), "%#x", v);
            break;
        }
        case '%':
            out_byte('%');
            break;
        default:
            // Unhandled conversion: the original emits the char literally.
            out_byte(c);
            break;
        }
        for (int k = 0; k < n && tmp[k]; k++) out_byte((uint8_t)tmp[k]);
    }
    MEM_B(count, dst) = 0; // wrapper NUL-terminates
    ctx->r2 = count;
    ctx->r3 = count;
}

// =============================================================================
// Native cart word read (game func_8008A710, its osPiReadIo)
// =============================================================================
// The loader (func_80011D60/func_80011BDC) reads the ROM asset table and
// records word-at-a-time through this primitive instead of PI DMA. Its body
// chains get-access (0x80093820) -> raw read (0x80093890) -> rel-access
// (0x80093864), all of which are stubbed, so every asset-table read returned
// garbage and the asset loader (func_800305F4) rejected every index, leaving
// func_8004611C to dereference the resulting NULL. Semantics: a0 = cart
// address (K1), a1 = guest u32* out, returns 0. Byte placement matches
// recomp::do_rom_pio (raw ROM bytes through MEM_B, same as the DMA path).
extern "C" void swe1r_pi_read_io(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t physical = ((uint32_t)ctx->r4) & 0x1FFFFFFF;
    const uint64_t out = ctx->r5; // sign-extended guest pointer
    std::span<const uint8_t> rom = recomp::get_rom();
    // The boot code that initializes the game's cart PI handle is stubbed, so
    // its baseAddress stays 0 and devAddr arrives as a raw ROM file offset
    // (the DMA redirect ORs rom_base in for the same reason). Accept both
    // domains: values below rom_base are already file offsets.
    const size_t off = (physical >= recomp::rom_base) ? (size_t)(physical - recomp::rom_base)
                                                      : (size_t)physical;
    if (off + 4 <= rom.size() && (off & 3) == 0 && ((uint32_t)out & 3) == 0) {
        recomp::do_rom_pio(rdram, out, (uint32_t)(off + recomp::rom_base));
    } else {
        fprintf(stderr, "[pi] unaligned/out-of-rom PIO read: devAddr=0x%08X out=0x%08X\n",
                (uint32_t)ctx->r4, (uint32_t)out);
        MEM_W(0, out) = 0;
    }
    ctx->r2 = 0;
}

// =============================================================================
// ROM Hash: Star Wars Episode I: Racer (US)
// CRC1: 0x72F70398  CRC2: 0x6556A98B
// =============================================================================
constexpr uint64_t SWE1R_ROM_HASH = 0x9B89D0F9EBC12D32ULL; // XXH3_64bits of baserom.z64

// =============================================================================
// Global state
// =============================================================================
static SDL_Window* g_window = nullptr;

// =============================================================================
// RSP Callbacks
// =============================================================================

// No-op RSP microcode. We don't yet have SWE1R's recompiled audio microcode
// (that would come from N64Recomp's RSPRecomp, like pokemonsnap's aspMain).
// Returning nullptr makes ultramodern's run_task abort the whole process, so
// instead we pretend audio tasks completed (RspExitReason::Broke). Result: no
// sound yet, but the game proceeds — and graphics tasks (M_GFXTASK) never reach
// here anyway; submit_rsp_task routes those straight to the RT64 renderer.
static RspExitReason swe1r_noop_ucode(uint8_t* /*rdram*/, uint32_t /*ucode_addr*/) {
    return RspExitReason::Broke;
}

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    static uint32_t rsp_call_count = 0;
    rsp_call_count++;
    if (rsp_call_count <= 5 || (rsp_call_count % 120 == 0)) {
        fprintf(stderr, "[SWE1R] get_rsp_microcode #%u: type=%u ucode=0x%08X data=0x%08X\n",
                rsp_call_count, task->t.type,
                (uint32_t)task->t.ucode, (uint32_t)task->t.ucode_data);
    }
    // Only non-graphics (audio) tasks reach here; no-op them for now.
    return swe1r_noop_ucode;
}

// =============================================================================
// Renderer Callbacks
// =============================================================================

std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context_callback(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return create_swe1r_render_context(rdram, window_handle, developer_mode);
}

// =============================================================================
// GFX Callbacks (window management)
// =============================================================================

ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "Failed to initialize SDL2: %s\n", SDL_GetError());
        std::exit(1);
    }

    printf("[SWE1R] SDL initialized: %s\n", SDL_GetCurrentVideoDriver());
    return nullptr;
}

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    uint32_t window_flags = SDL_WINDOW_RESIZABLE;
#ifdef RT64_SDL_WINDOW_VULKAN
    window_flags |= SDL_WINDOW_VULKAN;
#endif

    g_window = SDL_CreateWindow(
        "Star Wars Episode I: Racer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 960,
        window_flags
    );

    if (!g_window) {
        fprintf(stderr, "Failed to create window: %s\n", SDL_GetError());
        std::exit(1);
    }

    printf("[SWE1R] Window created (1280x960)\n");

#if defined(_WIN32)
    // Return native window handle
    SDL_SysWMinfo wminfo;
    SDL_VERSION(&wminfo.version);
    SDL_GetWindowWMInfo(g_window, &wminfo);

    ultramodern::renderer::WindowHandle handle{};
    handle.window = wminfo.info.win.window;
    handle.thread_id = GetCurrentThreadId();
    return handle;
#elif defined(RT64_SDL_WINDOW_VULKAN)
    // Linux: ultramodern's WindowHandle is the SDL_Window* itself.
    return ultramodern::renderer::WindowHandle{ g_window };
#else
    return ultramodern::renderer::WindowHandle{ g_window, nullptr };
#endif
}

static std::vector<SDL_GameController*> g_controllers;

static int8_t n64_axis(int16_t value) {
    constexpr int deadzone = 2600;
    if (value > -deadzone && value < deadzone) return 0;
    return (int8_t)std::clamp(value * 85 / 32767, -85, 85);
}

static HostPad sample_host_pad() {
    HostPad pad{};
    const uint8_t* keys = SDL_GetKeyboardState(nullptr);
    if (keys[SDL_SCANCODE_RETURN]) pad.buttons |= 0x8000;
    if (keys[SDL_SCANCODE_LSHIFT]) pad.buttons |= 0x4000;
    if (keys[SDL_SCANCODE_Z])      pad.buttons |= 0x2000;
    if (keys[SDL_SCANCODE_SPACE])  pad.buttons |= 0x1000;
    if (keys[SDL_SCANCODE_UP])     pad.buttons |= 0x0800;
    if (keys[SDL_SCANCODE_DOWN])   pad.buttons |= 0x0400;
    if (keys[SDL_SCANCODE_LEFT])   pad.buttons |= 0x0200;
    if (keys[SDL_SCANCODE_RIGHT])  pad.buttons |= 0x0100;
    int stick_x = (keys[SDL_SCANCODE_D] ? 85 : 0) - (keys[SDL_SCANCODE_A] ? 85 : 0);
    int stick_y = (keys[SDL_SCANCODE_W] ? 85 : 0) - (keys[SDL_SCANCODE_S] ? 85 : 0);

    constexpr std::pair<SDL_GameControllerButton, uint16_t> button_map[] = {
        { SDL_CONTROLLER_BUTTON_A, 0x8000 },
        { SDL_CONTROLLER_BUTTON_X, 0x4000 },
        { SDL_CONTROLLER_BUTTON_START, 0x1000 },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, 0x0800 },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x0400 },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x0200 },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x0100 },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x0020 },
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x0010 },
    };
    constexpr int16_t trigger_threshold = 16384;
    for (SDL_GameController* controller : g_controllers) {
        for (const auto& [button, mask] : button_map) {
            if (SDL_GameControllerGetButton(controller, button)) pad.buttons |= mask;
        }
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > trigger_threshold) pad.buttons |= 0x2000;
        if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > trigger_threshold) pad.buttons |= 0x0010;
        const int16_t right_x = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX);
        const int16_t right_y = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY);
        if (right_y < -trigger_threshold) pad.buttons |= 0x0008;
        if (right_y > trigger_threshold) pad.buttons |= 0x0004;
        if (right_x < -trigger_threshold) pad.buttons |= 0x0002;
        if (right_x > trigger_threshold) pad.buttons |= 0x0001;
        stick_x += n64_axis(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX));
        stick_y -= n64_axis(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY));
    }
    pad.stick_x = (int8_t)std::clamp(stick_x, -85, 85);
    pad.stick_y = (int8_t)std::clamp(stick_y, -85, 85);
    return pad;
}

void update_gfx(ultramodern::gfx_callbacks_t::gfx_data_t) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                ultramodern::quit();
                break;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE) {
                    ultramodern::quit();
                }
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (SDL_GameController* controller = SDL_GameControllerOpen(event.cdevice.which)) {
                    g_controllers.push_back(controller);
                    printf("[input] controller opened: %s\n", SDL_GameControllerName(controller));
                }
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                std::erase_if(g_controllers, [&](SDL_GameController* controller) {
                    if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) != event.cdevice.which) return false;
                    printf("[input] controller closed: %s\n", SDL_GameControllerName(controller));
                    SDL_GameControllerClose(controller);
                    return true;
                });
                break;
        }
    }
    const uint32_t packed = pack_host_pad(sample_host_pad());
    if (g_host_pad.exchange(packed, std::memory_order_relaxed) != packed) {
        const HostPad pad = read_host_pad();
        printf("[input] host pad buttons=0x%04X stick=%d,%d\n", pad.buttons, pad.stick_x, pad.stick_y);
    }
}

void poll_input() {
}

bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0) return false;
    const HostPad pad = read_host_pad();
    *buttons = pad.buttons;
    *x = pad.stick_x / 85.0f;
    *y = pad.stick_y / 85.0f;
    return true;
}

void set_rumble(int controller_num, bool rumble) {
    // TODO: SDL haptic feedback
}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (controller_num == 0) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

// =============================================================================
// Event Callbacks
// =============================================================================

void vi_callback() {
    // Called every VI interrupt
}

void gfx_init_callback() {
    printf("[SWE1R] Graphics initialized\n");
    // Start the game now that the renderer and VI thread are ready.
    // This must happen after start() has begun, not before, so the VI
    // thread gets a chance to set dummy VI state before is_game_started() is true.
    std::u8string game_id = u8"swe1racer";
    recomp::start_game(game_id, "default");
    printf("[SWE1R] Game started\n");
}

// =============================================================================
// Error Handling
// =============================================================================

void error_message_box(const char* msg) {
    fprintf(stderr, "[SWE1R ERROR] %s\n", msg);
    if (g_window) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SWE1R Error", msg, g_window);
    }
}

// =============================================================================
// Thread Naming
// =============================================================================

std::string get_game_thread_name(const OSThread* t) {
    return "SWE1R-Thread";
}

// =============================================================================
// Game Init Callback - initialize libultra state that stubbed boot code skips
// =============================================================================

void on_game_init(uint8_t* rdram, recomp_context* ctx) {
    // Capture the RDRAM base so the crash handler can map native fault
    // addresses back to N64 virtual addresses.
    g_rdram_base = rdram;

    // The game's boot code at 0x8008D284 initializes the PI (cartridge ROM) handle
    // and stores it at globals 0x800A7BC0 and 0x800A7FC0. That code is stubbed
    // because it contains COP0 instructions. We replicate the initialization here.

    // Initialize the PI handle struct at the pre-allocated RDRAM address
    int32_t handle_addr = recomp::cart_handle; // 0x80800000
    OSPiHandle* handle = (OSPiHandle*)(rdram + ((uint32_t)handle_addr - 0x80000000u));
    memset(handle, 0, sizeof(OSPiHandle));
    handle->type = 0; // PI_DOMAIN2 (cartridge)
    handle->baseAddress = 0xB0000000; // phys_to_k1(0x10000000) = cartridge ROM base
    handle->domain = 0;

    // Store the handle pointer in the game's global variables (big-endian u32 writes)
    // 0x800A7BC0 = __osCartRomHandle (used by PI access functions)
    // 0x800A7FC0 = __osPiTable (PI device linked list head)
    auto write32 = [&](uint32_t mips_addr, uint32_t value) {
        *(uint32_t*)(rdram + (mips_addr - 0x80000000u)) = value;
    };

    write32(0x800A7BC0, (uint32_t)handle_addr);
    write32(0x800A7FC0, (uint32_t)handle_addr);

    // Self-link the handle's next pointer (linked list of one element)
    // handle_addr = 0x80800000, so this writes to RDRAM offset 0x00800000 (8MB)
    write32((uint32_t)handle_addr, (uint32_t)handle_addr); // handle->unused (next ptr) = self

    printf("[SWE1R] PI handle initialized at 0x%08X\n", (uint32_t)handle_addr);

    // Initialize VI state for the scheduler thread.
    // The scheduler (func_8008BC30) calls static_0_800941D0 which reads a pointer
    // from 0x800A7F50 pointing to the "current VI mode" struct. It uses:
    //   +0x02 (halfword): retrace count — how many VI retraces before sending a response
    //   +0x10 (word): response queue — where to send the "frame done" message
    // osCreateViManager normally sets this up, but it's stubbed in the recomp.
    // Without this, the scheduler sends responses to its own queue (feedback loop)
    // and never yields to the game thread.
    //
    // We create a fake VI state at 0x800A7F00 (unused BSS near the pointer) and a
    // "sink" queue at 0x800A7E80 that absorbs the scheduler's responses.

    // Create the sink queue: osCreateMesgQueue(queue=0x800A7E80, buf=0x800A7E60, count=8)
    // We can't call osCreateMesgQueue_recomp here (no ctx), so write the struct directly.
    // OSMesgQueue layout (big-endian):
    //   +0x00: mtqueue (PTR) = 0 (unused)
    //   +0x04: fullqueue (PTR) = 0 (unused)
    //   +0x08: validCount (s32) = 0
    //   +0x0C: first (s32) = 0
    //   +0x10: msgCount (s32) = 8
    //   +0x14: msg (PTR) = 0x800A7E60 (buffer)
    write32(0x800A7E80 + 0x00, 0);
    write32(0x800A7E80 + 0x04, 0);
    write32(0x800A7E80 + 0x08, 0);
    write32(0x800A7E80 + 0x0C, 0);
    write32(0x800A7E80 + 0x10, 8);
    write32(0x800A7E80 + 0x14, 0x800A7E60);

    // Create fake VI state at 0x800A7F00
    // RDRAM uses native word order with XOR byte-swapping for sub-word access.
    // MEM_W reads/writes uint32_t directly. MEM_HU(0x02, base) reads the lower
    // halfword of the word at base. So to set the halfword at +0x02 = 1,
    // we write the word at +0x00 = 0x00000001 (lower half = 1, upper half = 0).
    memset(rdram + (0x800A7F00 - 0x80000000u), 0, 0x20);
    write32(0x800A7F00, 0x00000001);  // +0x00 word: lower halfword (+0x02) = retrace count 1
    write32(0x800A7F10, 0x800A7E80);  // +0x10: response queue = sink queue

    // Point the VI state pointer to our struct
    write32(0x800A7F50, 0x800A7F00);
    printf("[SWE1R] VI state initialized at 0x800A7F00, sink queue at 0x800A7E80\n");

    // osMemSize at 0x80000318 — IPL3 normally writes the detected RDRAM size
    // here, but we stub IPL3. SWE1R requires the Expansion Pak, so report 8MB.
    // The framebuffer allocator (func_80039A38) reads this to size buffers.
    // 0x318 is below the game's BSS clear (0x520+), so this value survives;
    // the framebuffer pointer table at 0x80114530 must NOT be seeded here, as
    // it sits inside the cleared range and is populated by func_80039A38.
    write32(0x80000318, 0x00800000);
    printf("[SWE1R] osMemSize=0x800000 written at 0x80000318\n");
}

#ifdef _WIN32
// =============================================================================
// Map-file symbolizer
//
// Release builds have no useful PDB symbols for the recompiled funcs (SymFromAddr
// returns garbage like "wcsrchr"). The linker .map file, however, lists every
// func_XXXX with its preferred VA. We parse it at startup and resolve crash
// stack frames against it — automating the manual map lookup used during Phase 5.
// =============================================================================
struct MapSym { uint32_t rva; std::string name; };
static std::vector<MapSym> g_map_syms;
static constexpr uint64_t MAP_PREFERRED_BASE = 0x140000000ull;

static bool is_hex16_va(const std::string& s) {
    if (s.size() != 16 || s.compare(0, 10, "0000000140") != 0) return false;
    for (char c : s) if (!isxdigit((unsigned char)c)) return false;
    return true;
}

static void load_map_symbols() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::filesystem::path mapPath = std::filesystem::path(exePath).replace_extension(".map");
    std::ifstream f(mapPath);
    if (!f) {
        fprintf(stderr, "[map] no symbol map at %s (crash traces won't be symbolized)\n",
                mapPath.string().c_str());
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        // Symbol lines look like: " 0001:00025e00   <name>   0000000140026e00 f i x.obj"
        std::istringstream iss(line);
        std::vector<std::string> tok;
        for (std::string t; iss >> t; ) tok.push_back(t);
        if (tok.size() < 3) continue;
        // tok[0] must be SECTION:OFFSET
        if (tok[0].size() != 13 || tok[0][4] != ':') continue;
        for (size_t i = 2; i < tok.size(); i++) {
            if (is_hex16_va(tok[i])) {
                uint64_t va = strtoull(tok[i].c_str(), nullptr, 16);
                g_map_syms.push_back({ (uint32_t)(va - MAP_PREFERRED_BASE), tok[i - 1] });
                break;
            }
        }
    }
    std::sort(g_map_syms.begin(), g_map_syms.end(),
              [](const MapSym& a, const MapSym& b) { return a.rva < b.rva; });
    fprintf(stderr, "[map] loaded %zu symbols from %s\n",
            g_map_syms.size(), mapPath.filename().string().c_str());
}

// Returns "name + 0xNNN" for a module-relative address, or empty if unknown.
static std::string symbolize_rva(uint32_t rva) {
    if (g_map_syms.empty()) return {};
    auto it = std::upper_bound(g_map_syms.begin(), g_map_syms.end(), rva,
                               [](uint32_t v, const MapSym& m) { return v < m.rva; });
    if (it == g_map_syms.begin()) return {};
    --it;
    char buf[320];
    snprintf(buf, sizeof(buf), "%s + 0x%X", it->name.c_str(), rva - it->rva);
    return buf;
}

// =============================================================================
// Main
// =============================================================================

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    fprintf(stderr, "\n[CRASH] Exception 0x%08lX at address 0x%p\n",
            ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        fprintf(stderr, "[CRASH] Access violation %s address 0x%p\n",
                ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (void*)fault);
        // Translate the fault back into an N64 address using the RDRAM base.
        if (g_rdram_base) {
            int64_t off = (int64_t)(fault - (uintptr_t)g_rdram_base);
            // MEM_B uses XOR 3 byte-swizzling, so undo it for the printed addr.
            uint32_t n64 = (uint32_t)(off + 0x80000000);
            uint32_t n64_b = (uint32_t)((off ^ 3) + 0x80000000);
            const char* tag = (off >= -0x10000 && off < 0x00900000)
                              ? "in RDRAM" : "out of RDRAM";
            fprintf(stderr, "[CRASH] rdram_base=%p offset=%+lld (%s)\n",
                    (void*)g_rdram_base, (long long)off, tag);
            fprintf(stderr, "[CRASH] -> N64 word addr ~0x%08X  byte addr ~0x%08X\n",
                    n64, n64_b);
        } else {
            fprintf(stderr, "[CRASH] rdram_base not set yet\n");
        }
    }
    // Print module + offset for the crash address
    HMODULE hMod = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &hMod);
    if (hMod) {
        char modName[MAX_PATH];
        GetModuleFileNameA(hMod, modName, MAX_PATH);
        uintptr_t offset = (uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)hMod;
        fprintf(stderr, "[CRASH] Module: %s + 0x%llX\n", modName, (unsigned long long)offset);
    }
    // Walk the stack and symbolize each frame against the linker .map. This
    // resolves the recompiled func_XXXX names that PDB-based SymFromAddr cannot.
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    uintptr_t modBase = (uintptr_t)GetModuleHandle(NULL);
    STACKFRAME64 frame = {};
    CONTEXT ctx = *ep->ContextRecord;
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    fprintf(stderr, "[CRASH] Stack trace (symbolized via .map):\n");
    for (int i = 0; i < 24; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(),
                         GetCurrentThread(), &frame, &ctx, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL))
            break;
        uint64_t pc = frame.AddrPC.Offset;
        std::string name;
        if (pc >= modBase) name = symbolize_rva((uint32_t)(pc - modBase));
        if (!name.empty())
            fprintf(stderr, "  [%d] %s   (rva 0x%X)\n", i, name.c_str(),
                    (uint32_t)(pc - modBase));
        else
            fprintf(stderr, "  [%d] 0x%llX\n", i, (unsigned long long)pc);
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#if !defined(_WIN32)
// POSIX equivalent of the Windows crash handler: prints the fault address, maps
// it back into N64 address space when it lands in RDRAM, and dumps a symbolized
// stack (build with -rdynamic so the recompiled func_XXXX names resolve).
static void posix_crash_handler(int sig, siginfo_t* info, void* /*ucontext*/) {
    void* frames[96];
    const int count = backtrace(frames, 96);
    const void* fault = (info != nullptr) ? info->si_addr : nullptr;

    fprintf(stderr, "\n[CRASH] signal %d (%s) at fault address %p\n", sig, strsignal(sig), fault);
    if (g_rdram_base != nullptr && fault != nullptr) {
        const int64_t off = static_cast<int64_t>(reinterpret_cast<uintptr_t>(fault) - reinterpret_cast<uintptr_t>(g_rdram_base));
        if (off >= -0x10000 && off < 0x00900000) {
            fprintf(stderr, "[CRASH] in RDRAM at offset %+lld (N64 byte addr ~0x%08X)\n",
                    static_cast<long long>(off), static_cast<unsigned>(off + 0x80000000));
        }
        else {
            fprintf(stderr, "[CRASH] outside RDRAM (rdram_base=%p, offset %+lld)\n",
                    static_cast<void*>(g_rdram_base), static_cast<long long>(off));
        }
    }
    fprintf(stderr, "[CRASH] native stack:\n");
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    fflush(stderr);
    _exit(128 + sig);
}
#endif

int main(int argc, char* argv[]) {
    // Force unbuffered output so we see prints before crashes
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
#ifdef _WIN32
    load_map_symbols();
    SetUnhandledExceptionFilter(crash_handler);
#endif

#if !defined(_WIN32)
    {
        struct sigaction sa{};
        sa.sa_sigaction = posix_crash_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sigemptyset(&sa.sa_mask);
        for (int crash_sig : { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE }) {
            sigaction(crash_sig, &sa, nullptr);
        }
    }
#endif

    fprintf(stderr, "[DEBUG] main() entered\n");

    printf("===========================================\n");
    printf("  Star Wars Episode I: Racer\n");
    printf("  N64 Static Recompilation v%d.%d.%d\n", 0, 1, 0);
    printf("  \"Now THIS is podracing!\"\n");
    printf("===========================================\n\n");

    // Register game entry
    recomp::GameEntry game_entry{};
    game_entry.rom_hash = SWE1R_ROM_HASH;
    game_entry.internal_name = "STAR WARS EP1 RACER";
    game_entry.display_name = "Star Wars Episode I: Racer";
    game_entry.game_id = u8"swe1racer";
    game_entry.mod_game_id = "swe1racer";
    game_entry.save_type = recomp::SaveType::Eep16k;
    game_entry.is_enabled = true;
    game_entry.entrypoint_address = (gpr)(int32_t)0x80000400; // Sign-extend for MEM macros
    game_entry.entrypoint = recomp_entrypoint;
    game_entry.on_init_callback = on_game_init;

    // Register overlay/section tables
    recomp::overlays::overlay_section_table_data_t sections_data{};
    sections_data.code_sections = section_table;
    sections_data.num_code_sections = num_sections;
    sections_data.total_num_sections = num_sections;

    recomp::overlays::overlays_by_index_t overlays_data{};
    overlays_data.table = nullptr;
    overlays_data.len = 0;

    recomp::overlays::register_overlays(sections_data, overlays_data);

    if (!recomp::register_game(game_entry)) {
        fprintf(stderr, "Failed to register game!\n");
        return 1;
    }

    printf("[SWE1R] Game registered: %zu sections, %zu functions in main section\n",
           num_sections, section_table[0].num_funcs);

    // Set up config path
    recomp::register_config_path(std::filesystem::current_path());

    // The runtime reloads the ROM from the config folder when the game starts,
    // so store a validated copy there when it is missing.
    recomp::check_all_stored_roms();
    if (!recomp::is_rom_valid(u8"swe1racer")) {
        const recomp::RomValidationError rom_err =
            recomp::select_rom(std::filesystem::path{"baserom.z64"}, u8"swe1racer");
        if (rom_err != recomp::RomValidationError::Good) {
            fprintf(stderr, "[SWE1R] ROM validation failed (code %d); expected baserom.z64 in the working directory.\n",
                    static_cast<int>(rom_err));
            return 1;
        }
        printf("[SWE1R] Stored ROM validated and copied into the config folder\n");
    }

    // Build the configuration
    recomp::Configuration cfg{};

    // VI is a 60 Hz periodic event: when the game's VI queue is full, the correct
    // behaviour (and real hardware's) is to drop the message, not requeue it.
    // Requeueing VI turned the external-message pump into a busy-spin livelock
    // (24M failed redeliveries in 10s) that starved the boot timer message and
    // deadlocked the game's first delay wait. One-shot sources still requeue.
    cfg.message_queue_control.requeue_timer = true;
    cfg.message_queue_control.requeue_sp = true;
    cfg.message_queue_control.requeue_si = true;
    cfg.message_queue_control.requeue_ai = true;
    cfg.message_queue_control.requeue_vi = false;
    cfg.message_queue_control.requeue_pi = true;
    cfg.message_queue_control.requeue_dp = true;
    cfg.project_version = { 0, 1, 0, "-alpha" };

    // RSP callbacks
    recomp::rsp::callbacks_t rsp_callbacks{};
    rsp_callbacks.get_rsp_microcode = get_rsp_microcode;
    cfg.rsp_callbacks = rsp_callbacks;

    // Renderer callbacks
    ultramodern::renderer::callbacks_t renderer_callbacks{};
    renderer_callbacks.create_render_context = create_render_context_callback;
    cfg.renderer_callbacks = renderer_callbacks;

    // Audio callbacks
    ultramodern::audio_callbacks_t audio_callbacks{};
    audio_callbacks.queue_samples = swe1r_queue_samples;
    audio_callbacks.get_frames_remaining = swe1r_get_frames_remaining;
    audio_callbacks.set_frequency = swe1r_set_frequency;
    cfg.audio_callbacks = audio_callbacks;

    // Input callbacks
    ultramodern::input::callbacks_t input_callbacks{};
    input_callbacks.poll_input = poll_input;
    input_callbacks.get_input = get_input;
    input_callbacks.set_rumble = set_rumble;
    input_callbacks.get_connected_device_info = get_connected_device_info;
    cfg.input_callbacks = input_callbacks;

    // GFX callbacks (window management)
    ultramodern::gfx_callbacks_t gfx_callbacks{};
    gfx_callbacks.create_gfx = create_gfx;
    gfx_callbacks.create_window = create_window;
    gfx_callbacks.update_gfx = update_gfx;
    cfg.gfx_callbacks = gfx_callbacks;

    // Events callbacks
    ultramodern::events::callbacks_t events_callbacks{};
    events_callbacks.vi_callback = vi_callback;
    events_callbacks.gfx_init_callback = gfx_init_callback;
    cfg.events_callbacks = events_callbacks;

    // Error handling
    ultramodern::error_handling::callbacks_t error_callbacks{};
    error_callbacks.message_box = error_message_box;
    cfg.error_handling_callbacks = error_callbacks;

    // Threads
    ultramodern::threads::callbacks_t threads_callbacks{};
    threads_callbacks.get_game_thread_name = get_game_thread_name;
    cfg.threads_callbacks = threads_callbacks;

    printf("[SWE1R] Starting runtime...\n");
    printf("  \"It's working! IT'S WORKING!\" - Anakin Skywalker\n\n");

    // Game is started from gfx_init_callback after renderer is ready.
    // This blocks until the game exits.
    recomp::start(cfg);

    // Cleanup
    if (g_window) {
        SDL_DestroyWindow(g_window);
    }
    SDL_Quit();

    printf("\n[SWE1R] Shutdown complete.\n");
    return 0;
}
