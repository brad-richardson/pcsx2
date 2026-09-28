// GE1's private PCSX2 GS host. This adapter calls public GS.h entry points.
#include "ge1_gs.h"
#include "pcsx2/GS.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
alignas(16) std::array<u8, 8192> s_priv{};
std::vector<u32> s_pixels;
MemorySettingsInterface s_settings;
bool s_open = false;
// PW1: pipeline-compile accounting (GE1_PIPE_STATS_CSV), periodic pipeline-cache
// flush (GE1_PIPE_FLUSH_VSYNCS) and TFX selector pre-warm (GE1_TFX_PREWARM).
// All default off; the GS thread is the only writer.
std::FILE* s_stats = nullptr;
std::uint64_t s_vsyncs = 0;
std::uint64_t s_flush_every = 0;
std::string s_prewarm_path;
constexpr std::uint32_t kSelectorTakeBatch = 1024;

void persist_recorded_selectors()
{
    if (s_prewarm_path.empty())
        return;
    const std::uint32_t sel_size = GSGetTFXSelectorSize();
    if (sel_size == 0)
        return;
    std::FILE* f = std::fopen(s_prewarm_path.c_str(), "ab");
    if (!f)
    {
        Console.Error("PW1: cannot append TFX selectors to '%s'", s_prewarm_path.c_str());
        return;
    }
    std::vector<u8> buf(static_cast<std::size_t>(sel_size) * kSelectorTakeBatch);
    for (;;)
    {
        const std::uint32_t n = GSTakeRecordedTFXSelectors(buf.data(), kSelectorTakeBatch);
        if (n == 0)
            break;
        if (std::fwrite(buf.data(), sel_size, n, f) != n)
        {
            Console.Error("PW1: short write appending TFX selectors to '%s'", s_prewarm_path.c_str());
            break;
        }
    }
    std::fflush(f);
    std::fclose(f);
}
constexpr u32 kOffsets[20] = {
    0x0000, 0x0010, 0x0020, 0x0030, 0x0040, 0x0050, 0x0060,
    0x0070, 0x0080, 0x0090, 0x00a0, 0x00b0, 0x00c0, 0x00d0,
    0x00e0, 0x1000, 0xffffffff, 0x1010, 0x1040, 0x1080};
}

extern "C" GE1_API int ge1_gs_open(int blending_level)
{
    if (s_open || blending_level < 0 || blending_level > 5)
        return 0;
    Log::SetConsoleOutputLevel(LOGLEVEL_DEBUG);
    EmuFolders::SetAppRoot();
    if (const char* resources = std::getenv("GE1_GS_RESOURCES_DIR"); resources && *resources) {
        if (!FileSystem::DirectoryExists(resources))
            return 0;
        EmuFolders::Resources = resources;
    } else if (!EmuFolders::SetResourcesDirectory()) {
        return 0;
    }
    if (const char* data = std::getenv("GE1_GS_DATA_DIR"); data && *data)
        EmuConfig.CustomDataPath = data;
    if (!EmuFolders::SetDataDirectory(nullptr))
        return 0;
    EmuFolders::SetDefaults(s_settings);
    EmuFolders::LoadConfig(s_settings);
    if (!EmuFolders::EnsureFoldersExist())
        return 0;
    const std::string roboto_path =
        EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
    const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
    if (roboto_data.empty())
        return 0;
    std::vector<ImGuiManager::FontInfo> fonts;
    ImGuiManager::FontInfo fi{};
    fi.data = roboto_data;
    fonts.push_back(fi);
    ImGuiManager::SetFonts(std::move(fonts));
    Host::Internal::SetBaseSettingsLayer(&s_settings);

    auto config = EmuConfig.GS;
    // GI1: renderer select. GE1_RENDERER=metal opens the native Metal backend
    // (Mac/iOS); anything else keeps the Vulkan default — except where
    // Vulkan isn't compiled in (iOS), which defaults to Metal.
#if defined(__APPLE__) && !defined(ENABLE_VULKAN)
    GSRendererType renderer = GSRendererType::Metal;
#else
    GSRendererType renderer = GSRendererType::VK;
#endif
#ifdef __APPLE__
    if (const char* want = std::getenv("GE1_RENDERER"); want && *want)
        renderer = (std::strcmp(want, "metal") == 0) ? GSRendererType::Metal : GSRendererType::VK;
#endif
    config.Renderer = renderer;
    config.UpscaleMultiplier = 1.0f;
    config.AccurateBlendingUnit = static_cast<AccBlendLevel>(blending_level);
    config.HWDownloadMode = GSHardwareDownloadMode::Enabled;
    config.HWMipmap = true;
    config.UserHacks_TextureInsideRt = GSTextureInRtMode::InsideTargets;
    config.UserHacks_HalfPixelOffset = GSHalfPixelOffset::Special;
    config.UserHacks_NativeScaling = GSNativeScaling::Normal;
    config.FXAA = false;
    config.LoadTextureReplacements = false;
    config.ShadeBoost = false;
    // GE4: Adreno sw-blend workaround (menu font static). Env-gated, default off.
    if (const char* bmix = std::getenv("GE1_ADRENO_BLEND_MIX"); bmix && std::strcmp(bmix, "1") == 0)
        config.AdrenoPreferBlendMix = true;
    // GP2: ARMSX2 fast packed-vertex parse (STQRGBAXYZF2). Env-gated, default off.
    if (const char* vk = std::getenv("GE1_VERTEX_KICK"); vk && std::strcmp(vk, "1") == 0)
        config.VertexKickFastParse = true;
    // GI1: diagnostic fetch disable (matches the Simulator, which forces fetch
    // off because sim Metal rejects fetch pipelines). Default off.
    if (const char* nofetch = std::getenv("GE1_DISABLE_FETCH"); nofetch && std::strcmp(nofetch, "1") == 0)
        config.DisableFramebufferFetch = true;
    // GP3: full fused vertex-kick path (direct kick + scalar-outcode cull +
    // fused min/max + two-pass kernel). Env-gated, default off.
    if (const char* vk = std::getenv("GE1_VERTEX_KICK"); vk && std::strcmp(vk, "2") == 0)
        config.VertexKickFused = true;
    // GP6: GS front/back split. off (default) = single-threaded, no record
    // round-trip; inline = records executed on the calling thread (identity
    // rung); lockstep = back thread + drain per record (bisect rung);
    // pipelined = two-object split, true overlap.
    if (const char* bt = std::getenv("GE1_BACKTHREAD"); bt && *bt)
    {
        if (std::strcmp(bt, "inline") == 0)
            config.BackThreadMode = GSBackThreadMode::InlineRecords;
        else if (std::strcmp(bt, "lockstep") == 0)
            config.BackThreadMode = GSBackThreadMode::Lockstep;
        else if (std::strcmp(bt, "pipelined") == 0)
            config.BackThreadMode = GSBackThreadMode::Pipelined;
        else
            config.BackThreadMode = GSBackThreadMode::Off;
    }
#ifdef __ANDROID__
    // Odin/Adreno default: preserve destination reads through texture barriers
    // while avoiding the broken framebuffer-fetch path. The safe probe stays
    // available for diagnosis; the original profile is an explicit override.
    config.OverrideTextureBarriers = 1;
    config.DisableFramebufferFetch = true;
    if (const char* safe = std::getenv("GE1_ADRENO_SAFE"); safe && std::strcmp(safe, "1") == 0) {
        config.OverrideTextureBarriers = 0;
    } else if (const char* original = std::getenv("GE1_ADRENO_ORIGINAL"); original && std::strcmp(original, "1") == 0) {
        config.OverrideTextureBarriers = -1;
        config.DisableFramebufferFetch = false;
    }
#endif
    // GE7: exact Adreno destination reads (all platforms; the Mac gate runs it).
    // Default AUTO (Brad sign-off): split on Adreno, barrier road elsewhere.
    // The VK backend resolves AUTO from the actual GPU at device creation (it
    // owns the GPU identity) and logs the resolved mode once; Metal (Apple
    // GPUs only) resolves here to the barrier road. Explicit
    // GE1_ADRENO_DSTREAD=off|split|copy|passbreak overrides. split and copy
    // imply the copy road and win over the Android default above when set.
    {
        const char* dstread = std::getenv("GE1_ADRENO_DSTREAD");
        const bool is_auto = !dstread || !*dstread || std::strcmp(dstread, "auto") == 0;
        if (is_auto && renderer != GSRendererType::VK) {
            std::fprintf(stderr, "GE7: dstread=off (auto: non-Vulkan renderer)\n");
        } else if (is_auto) {
            config.AdrenoDstReadAuto = true;
        } else if (std::strcmp(dstread, "off") == 0) {
            std::fprintf(stderr, "GE7: dstread=off (explicit)\n");
        } else if (std::strcmp(dstread, "split") == 0) {
            config.OverrideTextureBarriers = 0;
            config.AdrenoDstReadSplit = true;
            std::fprintf(stderr, "GE7: dstread=split (explicit)\n");
        } else if (std::strcmp(dstread, "copy") == 0) {
            // Same as GE1_ADRENO_SAFE on Android, but available on every platform:
            // the single-snapshot copy road, for A/B against split.
            config.OverrideTextureBarriers = 0;
            std::fprintf(stderr, "GE7: dstread=copy (explicit)\n");
        } else if (std::strcmp(dstread, "passbreak") == 0) {
            // Barrier road's decisions; ordering through pass breaks, not barriers.
            config.AdrenoDstReadBreak = true;
            std::fprintf(stderr, "GE7: dstread=passbreak (explicit)\n");
        }
    }
    std::fill(s_priv.begin(), s_priv.end(), 0);
    s_open = GSopen(config, renderer, s_priv.data(), GSVSyncMode::Disabled, false);
    if (s_open)
    {
        // PW1 knobs, all default off. Errors here are non-fatal: the knobs are
        // diagnostics and pre-warm, never required for correct rendering.
        if (const char* csv = std::getenv("GE1_PIPE_STATS_CSV"); csv && *csv)
        {
            s_stats = std::fopen(csv, "w");
            if (s_stats)
            {
                std::fprintf(s_stats, "vsync,new_tfx,tfx_us,new_spv,spv_us,flush_us\n");
                std::fflush(s_stats);
            }
            else
            {
                Console.Error("PW1: cannot open GE1_PIPE_STATS_CSV '%s'", csv);
            }
        }
        if (const char* every = std::getenv("GE1_PIPE_FLUSH_VSYNCS"); every && *every)
            s_flush_every = std::strtoull(every, nullptr, 10);
        if (const char* prewarm = std::getenv("GE1_TFX_PREWARM"); prewarm && *prewarm)
        {
            s_prewarm_path = prewarm;
            if (s_flush_every == 0)
                s_flush_every = 600;
            GSSetTFXSelectorRecord(true);
            const std::uint32_t sel_size = GSGetTFXSelectorSize();
            std::ifstream in(prewarm, std::ios::binary | std::ios::ate);
            if (sel_size > 0 && in)
            {
                const std::size_t bytes = static_cast<std::size_t>(in.tellg());
                if (bytes > 0 && bytes <= 4u * 1024u * 1024u && (bytes % sel_size) == 0)
                {
                    const std::uint32_t count = static_cast<std::uint32_t>(bytes / sel_size);
                    std::vector<u8> blob(bytes);
                    in.seekg(0);
                    in.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(bytes));
                    if (in)
                    {
                        // Don't re-record what prewarm creates: the file already holds them.
                        GSSetTFXSelectorRecord(false);
                        const auto t0 = std::chrono::steady_clock::now();
                        const std::uint32_t created =
                            GSPrewarmTFXPipelines(blob.data(), count > 65536 ? 65536 : count);
                        GSSetTFXSelectorRecord(true);
                        const std::uint64_t us = static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0).count());
                        Console.WriteLn("PW1: prewarmed %u/%u TFX pipelines in %llu us from '%s'", created,
                            count, static_cast<unsigned long long>(us), prewarm);
                        if (s_stats)
                        {
                            std::fprintf(s_stats, "# prewarm selectors=%u created=%u us=%llu\n", count, created,
                                static_cast<unsigned long long>(us));
                            std::fflush(s_stats);
                        }
                    }
                }
                else if (bytes > 0)
                {
                    Console.Error("PW1: ignoring malformed selector file '%s' (%zu bytes)", prewarm, bytes);
                }
            }
        }
    }
    return s_open ? 1 : 0;
}

extern "C" GE1_API void ge1_gs_close(void)
{
    if (s_open)
    {
        persist_recorded_selectors();
        GSclose();
    }
    if (s_stats)
    {
        std::fclose(s_stats);
        s_stats = nullptr;
    }
    s_open = false;
    s_vsyncs = 0;
    s_flush_every = 0;
    s_prewarm_path.clear();
    s_pixels.clear();
}

extern "C" GE1_API int ge1_gs_reset(const uint64_t words[20], const uint8_t* vram, uint32_t size)
{
    if (!s_open || !words || !vram || size != 4u * 1024u * 1024u)
        return 0;
    // The A0 capture starts with zero VRAM. An arbitrary nonzero snapshot
    // needs a GSfreeze/defrost seed and is explicitly rejected here.
    for (uint32_t i = 0; i < size; i++)
        if (vram[i] != 0)
            return 0;
    GSreset(true);
    for (uint32_t i = 0; i < 20; i++)
        if (kOffsets[i] != 0xffffffff)
            std::memcpy(s_priv.data() + kOffsets[i], words + i, 8);
    return 1;
}

extern "C" GE1_API int ge1_gs_priv_write(uint32_t offset, uint64_t value)
{
    if (!s_open || offset > 8192u - 8u || (offset & 7u))
        return 0;
    // C2 records the resulting MMIO image. CSR command writes are not
    // distinguishable here, so mirror state without reissuing side effects.
    std::memcpy(s_priv.data() + offset, &value, 8);
    return 1;
}

extern "C" GE1_API int ge1_gs_packet(uint8_t path, const uint8_t* bytes, uint32_t size)
{
    if (!s_open || !bytes || !size || (size & 15u))
        return 0;
    const uint32_t qwc = size / 16u;
    switch (path) {
        case 1: GSgifTransfer(bytes, qwc); break;
        case 2: GSgifTransfer2(const_cast<u8*>(bytes), qwc); break;
        case 3: GSgifTransfer3(const_cast<u8*>(bytes), qwc); break;
        default: return 0;
    }
    return 1;
}

extern "C" GE1_API int ge1_gs_vsync(uint32_t field, uint64_t csr, uint64_t smode1, uint64_t syncv)
{
    if (!s_open || field > 1)
        return 0;
    std::memcpy(s_priv.data() + 0x1000, &csr, 8);
    std::memcpy(s_priv.data() + 0x0010, &smode1, 8);
    std::memcpy(s_priv.data() + 0x0060, &syncv, 8);
    GSvsync(field, true);
    s_vsyncs++;
    std::uint64_t flush_us = 0;
    if (s_flush_every > 0 && (s_vsyncs % s_flush_every) == 0)
    {
        const auto t0 = std::chrono::steady_clock::now();
        GSFlushPipelineCache();
        persist_recorded_selectors();
        flush_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count());
    }
    if (s_stats)
    {
        std::uint64_t new_tfx = 0, tfx_ns = 0, new_spv = 0, spv_ns = 0;
        GSGetAndResetPipelineStats(&new_tfx, &tfx_ns, &new_spv, &spv_ns);
        std::fprintf(s_stats, "%llu,%llu,%llu,%llu,%llu,%llu\n", static_cast<unsigned long long>(s_vsyncs),
            static_cast<unsigned long long>(new_tfx), static_cast<unsigned long long>(tfx_ns / 1000),
            static_cast<unsigned long long>(new_spv), static_cast<unsigned long long>(spv_ns / 1000),
            static_cast<unsigned long long>(flush_us));
        std::fflush(s_stats);
    }
    return 1;
}

extern "C" GE1_API int ge1_gs_read_fifo(uint8_t* bytes, uint32_t qwords)
{
    if (!s_open || !bytes || qwords > 1024u)
        return 0;
    GSInitAndReadFIFO(bytes, qwords);
    return 1;
}

extern "C" GE1_API int ge1_gs_snapshot(uint32_t* width, uint32_t* height, const uint32_t** rgba)
{
    if (!s_open || !width || !height || !rgba)
        return 0;
    if (!GSSaveSnapshotToMemory(640, 480, false, false, width, height, &s_pixels))
        return 0;
    *rgba = s_pixels.data();
    return 1;
}

extern "C" GE1_API int ge1_gs_export_ahb(void* buffer, uint32_t width, uint32_t height, uint64_t* fence_counter)
{
#ifdef __ANDROID__
    return s_open ? GSExportSnapshotToAHB(static_cast<AHardwareBuffer*>(buffer), width, height, fence_counter) : -1;
#else
    (void)buffer; (void)width; (void)height; (void)fence_counter;
    return 0;
#endif
}

extern "C" GE1_API void ge1_gs_wait_export(uint64_t fence_counter)
{
#ifdef __ANDROID__
    if (s_open) GSWaitExportFence(fence_counter);
#else
    (void)fence_counter;
#endif
}

extern "C" GE1_API void ge1_gs_release_ahb(void* buffer)
{
#ifdef __ANDROID__
    if (s_open) GSReleaseExportAHB(static_cast<AHardwareBuffer*>(buffer));
#else
    (void)buffer;
#endif
}

extern "C" GE1_API int ge1_gs_export_iosurface(void* iosurface, uint32_t width, uint32_t height,
                                               ge1_gs_export_done_fn done, void* ctx)
{
#ifdef __APPLE__
    if (!s_open || !done)
        return 0;
    return GSExportSnapshotToIOSurface(iosurface, width, height,
        reinterpret_cast<GSExportIOSurfaceDoneFn>(done), ctx);
#else
    (void)iosurface; (void)width; (void)height; (void)done; (void)ctx;
    return 0;
#endif
}

extern "C" GE1_API void ge1_gs_release_iosurface(void* iosurface)
{
#ifdef __APPLE__
    if (s_open) GSReleaseExportIOSurface(iosurface);
#else
    (void)iosurface;
#endif
}

extern "C" GE1_API float ge1_gs_gpu_ms(void)
{
    return s_open ? GSGetAndResetAccumulatedGPUTime() : -1.0f;
}

extern "C" GE1_API int ge1_gs_flush_caches(void)
{
    if (!s_open)
        return 0;
    GSFlushPipelineCache();
    persist_recorded_selectors();
    return 1;
}
