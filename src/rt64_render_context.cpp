// RT64 Render Context for Star Wars Episode I: Racer
// Bridges ultramodern's RendererContext with the RT64 renderer

// NOMINMAX must be set globally via CMake (target_compile_definitions)
// Include RT64 first to avoid Windows.h macro conflicts
#include "hle/rt64_application.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// stb_image_write's implementation is compiled by RT64 (rt64_rdp_tmem.cpp), so
// only the declarations are needed here.
#include "stb_image_write.h"

#include "ultramodern/renderer_context.hpp"
#include "librecomp/game.hpp"

// Static buffers for RT64 Core (RSP memory, not used in HLE mode but required)
static uint8_t s_dmem[0x1000];
static uint8_t s_imem[0x1000];

// DPC registers (dummy storage - RT64 HLE doesn't use these directly)
static uint32_t s_dpc_start = 0;
static uint32_t s_dpc_end = 0;
static uint32_t s_dpc_current = 0;
static uint32_t s_dpc_status = 0;
static uint32_t s_dpc_clock = 0;
static uint32_t s_dpc_bufbusy = 0;
static uint32_t s_dpc_pipebusy = 0;
static uint32_t s_dpc_tmem = 0;
static uint32_t s_mi_intr = 0;

static void dummy_check_interrupts() {
    // No-op: ultramodern handles interrupts
}

// Bring-up aid: write the guest framebuffer to a PNG so rendered content can be
// inspected without relying on the host window/compositor. Enabled by setting
// SWE1R_FB_DUMP to a file path prefix.
static void dump_guest_framebuffer(uint8_t* rdram, ultramodern::renderer::ViRegs* vi_regs,
                                   const char* prefix, uint32_t frame) {
    if (vi_regs == nullptr || prefix == nullptr) {
        return;
    }

    // VI_ORIGIN holds the physical RDRAM address of the framebuffer being scanned out.
    const uint32_t origin = vi_regs->VI_ORIGIN_REG & 0x3FFFFF;
    const uint32_t width = vi_regs->VI_WIDTH_REG;
    const uint32_t height = 240;

    if (width == 0 || width > 1024 || origin == 0 || origin + width * height * 2 > 0x800000) {
        return;
    }

    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            // Halfword reads go through the same XOR-2 swizzle as MEM_H in the recompiled code.
            const uint32_t offset = (origin + (y * width + x) * 2) ^ 2;
            const uint16_t pixel = *reinterpret_cast<const uint16_t*>(rdram + offset);
            const uint8_t r5 = (pixel >> 11) & 0x1F;
            const uint8_t g5 = (pixel >> 6) & 0x1F;
            const uint8_t b5 = (pixel >> 1) & 0x1F;
            uint8_t* dst = &rgb[(static_cast<size_t>(y) * width + x) * 3];
            dst[0] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
            dst[1] = static_cast<uint8_t>((g5 << 3) | (g5 >> 2));
            dst[2] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
        }
    }

    char path[512];
    snprintf(path, sizeof(path), "%s-%05u.png", prefix, frame);
    if (stbi_write_png(path, static_cast<int>(width), static_cast<int>(height), 3,
                       rgb.data(), static_cast<int>(width * 3)) != 0) {
        printf("[SWE1R] framebuffer dump frame %u: %ux%u at 0x%06X -> %s\n",
               frame, width, height, origin, path);
    }
}

class SWE1RRenderContext : public ultramodern::renderer::RendererContext {
public:
    SWE1RRenderContext(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode)
        : rdram_(rdram), window_handle_(window_handle), developer_mode_(developer_mode)
    {
        setup_result = ultramodern::renderer::SetupResult::Success;
        fb_dump_prefix_ = std::getenv("SWE1R_FB_DUMP");
#if defined(_WIN32)
        chosen_api = ultramodern::renderer::GraphicsApi::D3D12;
#elif defined(__APPLE__)
        chosen_api = ultramodern::renderer::GraphicsApi::Metal;
#else
        chosen_api = ultramodern::renderer::GraphicsApi::Vulkan;
#endif

        // Get VI register pointers from ultramodern
        vi_regs_ = ultramodern::renderer::get_vi_regs();

        // Set up the RT64 Core structure
        RT64::Application::Core core{};
#if defined(__linux__)
        // RT64 is built with RT64_SDL_WINDOW_VULKAN, so RenderWindow is the SDL_Window*.
        core.window = window_handle;
#else
        core.window = window_handle.window;
#endif
        core.RDRAM = rdram;
        core.DMEM = s_dmem;
        core.IMEM = s_imem;

        // ROM header (first 0x40 bytes)
        auto rom = recomp::get_rom();
        if (rom.size() >= 0x40) {
            rom_header_.assign(rom.begin(), rom.begin() + 0x40);
            core.HEADER = rom_header_.data();
        } else {
            rom_header_.resize(0x40, 0);
            core.HEADER = rom_header_.data();
        }

        // MI/DPC registers (dummy)
        core.MI_INTR_REG = &s_mi_intr;
        core.DPC_START_REG = &s_dpc_start;
        core.DPC_END_REG = &s_dpc_end;
        core.DPC_CURRENT_REG = &s_dpc_current;
        core.DPC_STATUS_REG = &s_dpc_status;
        core.DPC_CLOCK_REG = &s_dpc_clock;
        core.DPC_BUFBUSY_REG = &s_dpc_bufbusy;
        core.DPC_PIPEBUSY_REG = &s_dpc_pipebusy;
        core.DPC_TMEM_REG = &s_dpc_tmem;

        // VI registers - point into ultramodern's ViRegs struct
        core.VI_STATUS_REG = &vi_regs_->VI_STATUS_REG;
        core.VI_ORIGIN_REG = &vi_regs_->VI_ORIGIN_REG;
        core.VI_WIDTH_REG = &vi_regs_->VI_WIDTH_REG;
        core.VI_INTR_REG = &vi_regs_->VI_INTR_REG;
        core.VI_V_CURRENT_LINE_REG = &vi_regs_->VI_V_CURRENT_LINE_REG;
        core.VI_TIMING_REG = &vi_regs_->VI_TIMING_REG;
        core.VI_V_SYNC_REG = &vi_regs_->VI_V_SYNC_REG;
        core.VI_H_SYNC_REG = &vi_regs_->VI_H_SYNC_REG;
        core.VI_LEAP_REG = &vi_regs_->VI_LEAP_REG;
        core.VI_H_START_REG = &vi_regs_->VI_H_START_REG;
        core.VI_V_START_REG = &vi_regs_->VI_V_START_REG;
        core.VI_V_BURST_REG = &vi_regs_->VI_V_BURST_REG;
        core.VI_X_SCALE_REG = &vi_regs_->VI_X_SCALE_REG;
        core.VI_Y_SCALE_REG = &vi_regs_->VI_Y_SCALE_REG;

        core.checkInterrupts = dummy_check_interrupts;

        // Configure RT64 application
        RT64::ApplicationConfiguration app_config;
        app_config.appId = "swe1racer";
        app_config.detectDataPath = true;
        app_config.useConfigurationFile = true;

        // Create the RT64 Application
        printf("[SWE1R] Creating RT64 application...\n");
        app_ = std::make_unique<RT64::Application>(core, app_config);
        app_->enhancementConfig.presentation.removeBlackBorders = false;

        // Run setup (creates graphics device, swap chain, shader caches, etc.)
#if defined(_WIN32)
        auto result = app_->setup(window_handle.thread_id);
#else
        // threadId is only consumed by the Win32 message hook.
        auto result = app_->setup(0);
#endif

        switch (result) {
        case RT64::Application::SetupResult::Success:
            printf("[SWE1R] RT64 initialized successfully\n");
            is_valid_ = true;
            break;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound:
            fprintf(stderr, "[SWE1R] RT64: Dynamic libraries not found\n");
            setup_result = ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
            break;
        case RT64::Application::SetupResult::InvalidGraphicsAPI:
            fprintf(stderr, "[SWE1R] RT64: Invalid graphics API\n");
            setup_result = ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
            break;
        case RT64::Application::SetupResult::GraphicsAPINotFound:
            fprintf(stderr, "[SWE1R] RT64: Graphics API not found\n");
            setup_result = ultramodern::renderer::SetupResult::GraphicsAPINotFound;
            break;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound:
            fprintf(stderr, "[SWE1R] RT64: Graphics device not found\n");
            setup_result = ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
            break;
        }
    }

    ~SWE1RRenderContext() override {
        if (app_) {
            printf("[SWE1R] Destroying RT64 application\n");
            app_.reset();
        }
    }

    bool valid() override { return is_valid_; }

    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override {
        // TODO: forward config changes to RT64
        return true;
    }

    void enable_instant_present() override {
        // RT64 doesn't have a separate instant present mode
    }

    void send_dummy_workload(uint32_t fb_address) override {
        // ultramodern asks the renderer for a workload when the game swaps
        // framebuffers without submitting a display list. RT64 exposes no
        // equivalent entry point, so there is nothing to forward yet.
        (void)fb_address;
    }

    void send_dl(const OSTask* task) override {
        if (!app_ || !is_valid_) return;

        dl_count_++;
        if (dl_count_ <= 5 || (dl_count_ % 60 == 0)) {
            printf("[SWE1R] send_dl #%u: data_ptr=0x%08X ucode=0x%08X ucode_data=0x%08X type=%u\n",
                   dl_count_, (uint32_t)task->t.data_ptr, (uint32_t)task->t.ucode,
                   (uint32_t)task->t.ucode_data, task->t.type);
        }

        // Tell RT64's HLE interpreter which microcode/GBI to use BEFORE parsing
        // the display list. Without this the interpreter has no GBI command map
        // and crashes in processDisplayLists. ucode/ucode_data/data_ptr are MIPS
        // virtual addresses; mask to the RDRAM physical offset RT64 indexes by.
        uint32_t ucode      = (uint32_t)task->t.ucode      & 0x3FFFFFF;
        uint32_t ucode_data = (uint32_t)task->t.ucode_data & 0x3FFFFFF;
        uint32_t dl_address = (uint32_t)task->t.data_ptr   & 0x3FFFFFF;
        app_->interpreter->loadUCodeGBI(ucode, ucode_data, true);
        app_->processDisplayLists(rdram_, dl_address, 0, true);
    }

    void update_screen() override {
        if (!app_ || !is_valid_) return;

        screen_count_++;
        if (screen_count_ <= 5 || (screen_count_ % 60 == 0)) {
            printf("[SWE1R] update_screen #%u\n", screen_count_);
        }

        app_->updateScreen();

        if (fb_dump_prefix_ != nullptr && (screen_count_ % 120 == 0)) {
            dump_guest_framebuffer(rdram_, vi_regs_, fb_dump_prefix_, screen_count_);
        }
    }

    void shutdown() override {
        printf("[SWE1R] Render context shutdown\n");
        if (app_) {
            app_.reset();
        }
        is_valid_ = false;
    }

    uint32_t get_display_framerate() const override {
        return 60; // TODO: query from RT64/display
    }

    float get_resolution_scale() const override {
        return 1.0f;
    }

private:
    uint8_t* rdram_;
    ultramodern::renderer::WindowHandle window_handle_;
    bool developer_mode_;
    bool is_valid_ = false;
    ultramodern::renderer::ViRegs* vi_regs_ = nullptr;
    std::unique_ptr<RT64::Application> app_;
    std::vector<uint8_t> rom_header_;
    uint32_t dl_count_ = 0;
    uint32_t screen_count_ = 0;
    const char* fb_dump_prefix_ = nullptr;
};

// Factory function
std::unique_ptr<ultramodern::renderer::RendererContext> create_swe1r_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<SWE1RRenderContext>(rdram, window_handle, developer_mode);
}
