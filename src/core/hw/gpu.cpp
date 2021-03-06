// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include "common/common_types.h"

#include "core/settings.h"
#include "core/core.h"
#include "core/mem_map.h"

#include "core/hle/hle.h"
#include "core/hle/service/gsp_gpu.h"

#include "core/hw/gpu.h"

#include "video_core/command_processor.h"
#include "video_core/video_core.h"


namespace GPU {

Regs g_regs;

u32 g_cur_line = 0;         ///< Current vertical screen line
u64 g_last_line_ticks = 0;  ///< CPU tick count from last vertical screen line
u64 g_last_frame_ticks = 0; ///< CPU tick count from last frame

static u32 kFrameCycles = 0; ///< 268MHz / 60 frames per second
static u32 kFrameTicks  = 0; ///< Approximate number of instructions/frame

template <typename T>
inline void Read(T &var, const u32 raw_addr) {
    u32 addr = raw_addr - 0x1EF00000;
    u32 index = addr / 4;

    // Reads other than u32 are untested, so I'd rather have them abort than silently fail
    if (index >= Regs::NumIds() || !std::is_same<T,u32>::value) {
        ERROR_LOG(GPU, "unknown Read%lu @ 0x%08X", sizeof(var) * 8, addr);
        return;
    }

    var = g_regs[addr / 4];
}

template <typename T>
inline void Write(u32 addr, const T data) {
    addr -= 0x1EF00000;
    u32 index = addr / 4;

    // Writes other than u32 are untested, so I'd rather have them abort than silently fail
    if (index >= Regs::NumIds() || !std::is_same<T,u32>::value) {
        ERROR_LOG(GPU, "unknown Write%lu 0x%08X @ 0x%08X", sizeof(data) * 8, data, addr);
        return;
    }

    g_regs[index] = static_cast<u32>(data);

    switch (index) {

    // Memory fills are triggered once the fill value is written.
    // NOTE: This is not verified.
    case GPU_REG_INDEX_WORKAROUND(memory_fill_config[0].value, 0x00004 + 0x3):
    case GPU_REG_INDEX_WORKAROUND(memory_fill_config[1].value, 0x00008 + 0x3):
    {
        const bool is_second_filler = (index != GPU_REG_INDEX(memory_fill_config[0].value));
        const auto& config = g_regs.memory_fill_config[is_second_filler];

        // TODO: Not sure if this check should be done at GSP level instead
        if (config.address_start) {
            // TODO: Not sure if this algorithm is correct, particularly because it doesn't use the size member at all
            u32* start = (u32*)Memory::GetPointer(Memory::PhysicalToVirtualAddress(config.GetStartAddress()));
            u32* end = (u32*)Memory::GetPointer(Memory::PhysicalToVirtualAddress(config.GetEndAddress()));
            for (u32* ptr = start; ptr < end; ++ptr)
                *ptr = bswap32(config.value); // TODO: This is just a workaround to missing framebuffer format emulation

            DEBUG_LOG(GPU, "MemoryFill from 0x%08x to 0x%08x", config.GetStartAddress(), config.GetEndAddress());
        }
        break;
    }

    case GPU_REG_INDEX(display_transfer_config.trigger):
    {
        const auto& config = g_regs.display_transfer_config;
        if (config.trigger & 1) {
            u8* source_pointer = Memory::GetPointer(Memory::PhysicalToVirtualAddress(config.GetPhysicalInputAddress()));
            u8* dest_pointer = Memory::GetPointer(Memory::PhysicalToVirtualAddress(config.GetPhysicalOutputAddress()));

            for (u32 y = 0; y < config.output_height; ++y) {
                // TODO: Why does the register seem to hold twice the framebuffer width?
                for (u32 x = 0; x < config.output_width; ++x) {
                    struct {
                        int r, g, b, a;
                    } source_color = { 0, 0, 0, 0 };

                    switch (config.input_format) {
                    case Regs::PixelFormat::RGBA8:
                    {
                        // TODO: Most likely got the component order messed up.
                        u8* srcptr = source_pointer + x * 4 + y * config.input_width * 4;
                        source_color.r = srcptr[0]; // blue
                        source_color.g = srcptr[1]; // green
                        source_color.b = srcptr[2]; // red
                        source_color.a = srcptr[3]; // alpha
                        break;
                    }

                    default:
                        ERROR_LOG(GPU, "Unknown source framebuffer format %x", config.input_format.Value());
                        break;
                    }

                    switch (config.output_format) {
                    /*case Regs::PixelFormat::RGBA8:
                    {
                        // TODO: Untested
                        u8* dstptr = (u32*)(dest_pointer + x * 4 + y * config.output_width * 4);
                        dstptr[0] = source_color.r;
                        dstptr[1] = source_color.g;
                        dstptr[2] = source_color.b;
                        dstptr[3] = source_color.a;
                        break;
                    }*/

                    case Regs::PixelFormat::RGB8:
                    {
                        // TODO: Most likely got the component order messed up.
                        u8* dstptr = dest_pointer + x * 3 + y * config.output_width * 3;
                        dstptr[0] = source_color.r; // blue
                        dstptr[1] = source_color.g; // green
                        dstptr[2] = source_color.b; // red
                        break;
                    }

                    default:
                        ERROR_LOG(GPU, "Unknown destination framebuffer format %x", config.output_format.Value());
                        break;
                    }
                }
            }

            DEBUG_LOG(GPU, "DisplayTriggerTransfer: 0x%08x bytes from 0x%08x(%ux%u)-> 0x%08x(%ux%u), dst format %x",
                      config.output_height * config.output_width * 4,
                      config.GetPhysicalInputAddress(), config.input_width, config.input_height,
                      config.GetPhysicalOutputAddress(), config.output_width, config.output_height,
                      config.output_format.Value());
        }
        break;
    }

    // Seems like writing to this register triggers processing
    case GPU_REG_INDEX(command_processor_config.trigger):
    {
        const auto& config = g_regs.command_processor_config;
        if (config.trigger & 1)
        {
            u32* buffer = (u32*)Memory::GetPointer(Memory::PhysicalToVirtualAddress(config.GetPhysicalAddress()));
            u32 size = config.size << 3;
            Pica::CommandProcessor::ProcessCommandList(buffer, size);
        }
        break;
    }

    default:
        break;
    }
}

// Explicitly instantiate template functions because we aren't defining this in the header:

template void Read<u64>(u64 &var, const u32 addr);
template void Read<u32>(u32 &var, const u32 addr);
template void Read<u16>(u16 &var, const u32 addr);
template void Read<u8>(u8 &var, const u32 addr);

template void Write<u64>(u32 addr, const u64 data);
template void Write<u32>(u32 addr, const u32 data);
template void Write<u16>(u32 addr, const u16 data);
template void Write<u8>(u32 addr, const u8 data);

/// Update hardware
void Update() {
    auto& framebuffer_top = g_regs.framebuffer_config[0];
    u64 current_ticks = Core::g_app_core->GetTicks();

    // Update the frame after a certain number of CPU ticks have elapsed. This assumes that the
    // active frame in memory is always complete to render. There also may be issues with this
    // becoming out-of-synch with GSP synchrinization code (as follows). At this time, this seems to
    // be the most effective solution for both homebrew and retail applications. With retail, this
    // could be moved below (and probably would guarantee more accurate synchronization). However,
    // primitive homebrew relies on a vertical blank interrupt to happen inevitably (regardless of a
    // threading reschedule).

    if ((current_ticks - g_last_frame_ticks) > GPU::kFrameTicks) {
        VideoCore::g_renderer->SwapBuffers();
        g_last_frame_ticks = current_ticks;
    }

    // Synchronize GPU on a thread reschedule: Because we cannot accurately predict a vertical
    // blank, we need to simulate it. Based on testing, it seems that retail applications work more
    // accurately when this is signalled between thread switches.

    if (HLE::g_reschedule) {

        // Synchronize line...
        if ((current_ticks - g_last_line_ticks) >= GPU::kFrameTicks / framebuffer_top.height) {
            GSP_GPU::SignalInterrupt(GSP_GPU::InterruptId::PDC0);
            g_cur_line++;
            g_last_line_ticks = current_ticks;
        }

        // Synchronize frame...
        if (g_cur_line >= framebuffer_top.height) {
            g_cur_line = 0;
            GSP_GPU::SignalInterrupt(GSP_GPU::InterruptId::PDC1);
        }
    }
}

/// Initialize hardware
void Init() {
    kFrameCycles = 268123480 / Settings::values.gpu_refresh_rate;
    kFrameTicks  = kFrameCycles / 3;

    g_cur_line = 0;
    g_last_frame_ticks = g_last_line_ticks = Core::g_app_core->GetTicks();

    auto& framebuffer_top = g_regs.framebuffer_config[0];
    auto& framebuffer_sub = g_regs.framebuffer_config[1];

    // Setup default framebuffer addresses (located in VRAM)
    // .. or at least these are the ones used by system applets.
    // There's probably a smarter way to come up with addresses
    // like this which does not require hardcoding.
    framebuffer_top.address_left1  = 0x181E6000;
    framebuffer_top.address_left2  = 0x1822C800;
    framebuffer_top.address_right1 = 0x18273000;
    framebuffer_top.address_right2 = 0x182B9800;
    framebuffer_sub.address_left1  = 0x1848F000;
    //framebuffer_sub.address_left2  = unknown;
    framebuffer_sub.address_right1 = 0x184C7800;
    //framebuffer_sub.address_right2 = unknown;

    framebuffer_top.width = 240;
    framebuffer_top.height = 400;
    framebuffer_top.stride = 3 * 240;
    framebuffer_top.color_format = Regs::PixelFormat::RGB8;
    framebuffer_top.active_fb = 0;

    framebuffer_sub.width = 240;
    framebuffer_sub.height = 320;
    framebuffer_sub.stride = 3 * 240;
    framebuffer_sub.color_format = Regs::PixelFormat::RGB8;
    framebuffer_sub.active_fb = 0;

    NOTICE_LOG(GPU, "initialized OK");
}

/// Shutdown hardware
void Shutdown() {
    NOTICE_LOG(GPU, "shutdown OK");
}

} // namespace
