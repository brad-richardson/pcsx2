// GE1's private PCSX2 GS host. This adapter calls public GS.h entry points.
#include "ge1_gs.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include "common/MemorySettingsInterface.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

bool ge1_host_reserve_sw_code(); // ge1_host.cpp (CN1A)

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
std::chrono::steady_clock::time_point s_last_vsync{}; // SH1: stats CSV wall_us
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
// UR1: SSX 3's displayed frame height (NTSC 640x448, the src rect every
// export scales from). native = display height / this.
constexpr float kSourceHeight = 448.0f;

bool parse_size(const char* s, unsigned* w, unsigned* h)
{
    return s && std::sscanf(s, "%ux%u", w, h) == 2 && *w > 0 && *h > 0 && *w <= 8192 && *h <= 8192;
}

// UR1 knobs, default off (returns false and leaves config untouched):
//   GE1_UPSCALE=<float>|native|halfnative  internal resolution (PCSX2 UpscaleMultiplier;
//       native = display height / 448 from GE1_DISPLAY_SIZE=WxH, which the runtime sets
//       from the game rect; halfnative = half of that)
//   GE1_FXAA=1                             PCSX2 FXAA on the merged frame
//   GE1_CAS=<0..1>                         CAS sharpen-only on the export, that sharpness
//   PS2X_PRESENT_FILTER=sharp              sharp bilinear for the export's final scale
//   GE1_SNAPSHOT_SIZE=WxH                  snapshot size (replay crops at display size)
// The SSX 3 GameDB fixes that matter when upscaled (halfPixelOffset 2 = Special,
// nativeScaling 1 = Normal, textureInsideRT 1) are already set unconditionally.
bool configure_output(Pcsx2Config::GSOptions& config)
{
    bool on = false;
    if (const char* up = std::getenv("GE1_UPSCALE"); up && *up)
    {
        float scale = 0.0f;
        const bool native = std::strcmp(up, "native") == 0, half = std::strcmp(up, "halfnative") == 0;
        if (native || half)
        {
            unsigned dw = 0, dh = 0;
            if (parse_size(std::getenv("GE1_DISPLAY_SIZE"), &dw, &dh))
                scale = static_cast<float>(dh) / kSourceHeight * (half ? 0.5f : 1.0f);
            else
                std::fprintf(stderr, "UR1: GE1_UPSCALE=%s needs GE1_DISPLAY_SIZE=WxH; staying 1x\n", up);
        }
        else
        {
            scale = std::strtof(up, nullptr);
        }
        if (scale > 1.0f)
        {
            config.UpscaleMultiplier = std::min(scale, 8.0f);
            on = true;
        }
    }
    if (const char* fx = std::getenv("GE1_FXAA"); fx && std::strcmp(fx, "1") == 0)
    {
        config.FXAA = true;
        on = true;
    }
    if (const char* cas = std::getenv("GE1_CAS"); cas && *cas)
    {
        const float s = std::strtof(cas, nullptr);
        if (s > 0.0f)
        {
            config.ExportCAS = true;
            config.CAS_Sharpness = static_cast<u8>(std::lround(std::min(s, 1.0f) * 100.0f));
            on = true;
        }
    }
    if (const char* pf = std::getenv("PS2X_PRESENT_FILTER"); pf && std::strcmp(pf, "sharp") == 0)
    {
        config.ExportSharpBilinear = true;
        on = true;
    }
    if (std::getenv("GE1_SNAPSHOT_SIZE"))
        on = true;
    if (on)
        std::fprintf(stderr, "UR1: upscale=%.4f fxaa=%d cas=%s(%u) filter=%s aspect=stretch\n",
            config.UpscaleMultiplier, config.FXAA ? 1 : 0, config.ExportCAS ? "on" : "off",
            static_cast<unsigned>(config.CAS_Sharpness), config.ExportSharpBilinear ? "sharp" : "bilinear");
    return on;
}

// UR2 knobs, default off (config untouched when unset):
//   GE1_UPSCALE_FIX=<tok>[,<tok>...]  PCSX2 upscale fixes, applied only when
//       UpscaleMultiplier > 1 (1x stays byte-identical). Tokens: half-pixel
//       offset off|normal|special|aggressive|native|nativetex (GameDB default
//       special), round1|round2 (round sprite), align (align sprite X), none.
//   GE1_ANISO=2|4|8|16                 PCSX2 MaxAnisotropy (shader aniso on
//       non-mipmapped triangle draws)
//   GE1_TRILINEAR=off|ps2|forced       PCSX2 TriFilter (default automatic = ps2)
void configure_filtering(Pcsx2Config::GSOptions& config)
{
    const char* fix = std::getenv("GE1_UPSCALE_FIX");
    if (fix && *fix && config.UpscaleMultiplier > 1.0f)
    {
        const std::string s(fix);
        size_t pos = 0;
        while (pos <= s.size())
        {
            const size_t end = std::min(s.find(',', pos), s.size());
            const std::string t = s.substr(pos, end - pos);
            if (t == "off") config.UserHacks_HalfPixelOffset = GSHalfPixelOffset::Off;
            else if (t == "normal") config.UserHacks_HalfPixelOffset = GSHalfPixelOffset::Normal;
            else if (t == "special") config.UserHacks_HalfPixelOffset = GSHalfPixelOffset::Special;
            else if (t == "aggressive") config.UserHacks_HalfPixelOffset = GSHalfPixelOffset::SpecialAggressive;
            else if (t == "native") config.UserHacks_HalfPixelOffset = GSHalfPixelOffset::Native;
            else if (t == "nativetex") config.UserHacks_HalfPixelOffset = GSHalfPixelOffset::NativeWTexOffset;
            else if (t == "round1") config.UserHacks_RoundSprite = 1;
            else if (t == "round2") config.UserHacks_RoundSprite = 2;
            else if (t == "align") config.UserHacks_AlignSpriteX = true;
            else if (!t.empty() && t != "none") std::fprintf(stderr, "UR2: GE1_UPSCALE_FIX token '%s' ignored\n", t.c_str());
            pos = end + 1;
        }
        std::fprintf(stderr, "UR2: upscale fix %s: hpo=%d round=%d align=%d\n", fix,
            static_cast<int>(config.UserHacks_HalfPixelOffset), static_cast<int>(config.UserHacks_RoundSprite),
            config.UserHacks_AlignSpriteX ? 1 : 0);
    }
    if (const char* af = std::getenv("GE1_ANISO"); af && *af)
    {
        const int n = std::atoi(af);
        if (n == 2 || n == 4 || n == 8 || n == 16)
        {
            config.MaxAnisotropy = static_cast<u8>(n);
            std::fprintf(stderr, "UR2: anisotropic %dx\n", n);
        }
    }
    if (const char* tri = std::getenv("GE1_TRILINEAR"); tri && *tri)
    {
        if (std::strcmp(tri, "off") == 0) config.TriFilter = TriFiltering::Off;
        else if (std::strcmp(tri, "ps2") == 0) config.TriFilter = TriFiltering::PS2;
        else if (std::strcmp(tri, "forced") == 0) config.TriFilter = TriFiltering::Forced;
        std::fprintf(stderr, "UR2: trilinear %s (TriFilter=%d)\n", tri, static_cast<int>(config.TriFilter));
    }
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
#if defined(__APPLE__) && TARGET_OS_IPHONE
        // CN1A: on the ARMSX2 base, pcsx2/MacOSStubs.cpp (built for iOS) answers
        // CocoaTools::GetResourcePath() with nullopt and wins over GI1CocoaTools.mm
        // in libPCSX2.a, so SetResourcesDirectory() looks for <app>/resources. The
        // GI1 bundle is flat (fonts/ and default.metallib at the bundle root, which
        // is AppRoot), so use that.
        if (!FileSystem::DirectoryExists(Path::Combine(EmuFolders::AppRoot, "fonts").c_str()))
            return 0;
        EmuFolders::Resources = EmuFolders::AppRoot;
        std::fprintf(stderr, "CN1A: resources = app bundle root (flat GI1 bundle)\n");
#else
        return 0;
#endif
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
    if (const char* want = std::getenv("GE1_RENDERER"); want && *want)
    {
        // CN1A: GE1_RENDERER=sw opens PCSX2's software renderer (CN2's pixel
        // reference; it presents through the platform's preferred device, Metal
        // on Apple, so GE1_GS_RESOURCES_DIR needs the metallibs there).
        if (std::strcmp(want, "sw") == 0)
            renderer = GSRendererType::SW;
#ifdef __APPLE__
        else
            renderer = (std::strcmp(want, "metal") == 0) ? GSRendererType::Metal : GSRendererType::VK;
#endif
    }
    if (renderer == GSRendererType::SW && !ge1_host_reserve_sw_code())
    {
        std::fprintf(stderr, "CN1A: SW renderer needs the SW-rec code region; SysMemory reservation failed\n");
        return 0;
    }
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
    // UR1: internal resolution + output filters. All unset = today, byte-identical.
    if (configure_output(config))
    {
        // GE1 fills whatever export size the runtime asks for (the runtime owns
        // the aspect: 16:9 anamorphic). At a 4:3 export (640x480) Stretch and the
        // default auto 4:3 give the same rect.
        config.AspectRatio = AspectRatioType::Stretch;
        EmuConfig.CurrentAspectRatio = AspectRatioType::Stretch;
    }
    // UR2: upscale fixes (only > 1x), anisotropic + trilinear filtering. Unset = today.
    configure_filtering(config);
    // GE4: Adreno sw-blend workaround (menu font static). Env-gated, default off.
    if (const char* bmix = std::getenv("GE1_ADRENO_BLEND_MIX"); bmix && std::strcmp(bmix, "1") == 0)
        config.AdrenoPreferBlendMix = true;
    // CN1-C: GE1_ADRENO_OVERONE=split keeps the per-batch GE7 split for over-one blend-mix
    // promotions (exact path); unset = one pre-draw copy per such draw.
    if (const char* ov = std::getenv("GE1_ADRENO_OVERONE"); ov && std::strcmp(ov, "split") == 0)
        config.AdrenoOverOneSplit = true;
    // GS10: PCSX2's per-game draw-buffering hack (GameIndex drawBuffering; SSX 3's
    // 3-pass terrain interleave halves draws/passes, GS9). Output-only. Default off.
    if (const char* db = std::getenv("GE1_DRAW_BUFFERING"); db && std::strcmp(db, "1") == 0)
        config.UserHacks_DrawBuffering = true;
    // GI1: diagnostic fetch disable (matches the Simulator, which forces fetch
    // off because sim Metal rejects fetch pipelines). Default off.
    if (const char* nofetch = std::getenv("GE1_DISABLE_FETCH"); nofetch && std::strcmp(nofetch, "1") == 0)
        config.DisableFramebufferFetch = true;
    // GP6/BP1 on the ARMSX2 base (CN1A): ARMSX2 2.7.2 carries the GSBackQueue
    // split (our GP6 port is their code) as a plain on/off GSOptions::BackThread;
    // the inline/lockstep bisect rungs are gone. Unset or empty resolves to the
    // platform default (Android pipelined, BP1 Brad sign-off; Mac/iOS off); an
    // explicit value wins: pipelined = on, off = off, inline/lockstep = off.
    if (const char* bt = std::getenv("GE1_BACKTHREAD"); !bt || !*bt)
    {
#ifdef __ANDROID__
        config.BackThread = true;
#endif
    }
    else
    {
        config.BackThread = std::strcmp(bt, "pipelined") == 0;
        if (std::strcmp(bt, "inline") == 0 || std::strcmp(bt, "lockstep") == 0)
            std::fprintf(stderr, "GP6: GE1_BACKTHREAD=%s has no ARMSX2 equivalent; back thread off\n", bt);
    }
    // GP2/GP3 on the ARMSX2 base: the fused vertex kick is ARMSX2's always-on
    // kick, so GE1_VERTEX_KICK has nothing left to select.
    if (const char* vk = std::getenv("GE1_VERTEX_KICK"); vk && *vk && std::strcmp(vk, "2") != 0)
        std::fprintf(stderr, "GP3: GE1_VERTEX_KICK=%s ignored (ARMSX2 base: fused kick always on)\n", vk);
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
    // FX2: terrain glint (Cs*Ad + Cd, alpha masked) through fixed-function
    // DST_ALPHA on an RT-alpha-scaled target instead of a dst read. Output-only
    // approximation (+-1 LSB on glint pixels). Env-gated, default off.
    if (const char* accu = std::getenv("GE1_ADRENO_AD_ACCU"); accu && std::strcmp(accu, "rta") == 0) {
        config.AdrenoAdAccuRta = true;
        std::fprintf(stderr, "FX2: ad_accu=rta\n");
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
                std::fprintf(s_stats, "vsync,new_tfx,tfx_us,new_spv,spv_us,flush_us,tfx_slow,tfx_max_us,up_kb,uploads,tex_new,tex_new_us,wall_us\n");
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
    s_last_vsync = {};
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
        // SH1: stall tags and the wall time since the previous vsync on this thread (the
        // back thread's creates/uploads can land one vsync late in pipelined mode).
        std::uint64_t st[6];
        GSGetAndResetStallStats(st);
        const auto now = std::chrono::steady_clock::now();
        const std::uint64_t wall_us = s_last_vsync.time_since_epoch().count() == 0 ? 0 :
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now - s_last_vsync).count());
        s_last_vsync = now;
        std::fprintf(s_stats, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
            static_cast<unsigned long long>(s_vsyncs),
            static_cast<unsigned long long>(new_tfx), static_cast<unsigned long long>(tfx_ns / 1000),
            static_cast<unsigned long long>(new_spv), static_cast<unsigned long long>(spv_ns / 1000),
            static_cast<unsigned long long>(flush_us), static_cast<unsigned long long>(st[0]),
            static_cast<unsigned long long>(st[1] / 1000), static_cast<unsigned long long>(st[2] / 1024),
            static_cast<unsigned long long>(st[3]), static_cast<unsigned long long>(st[4]),
            static_cast<unsigned long long>(st[5] / 1000), static_cast<unsigned long long>(wall_us));
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
    // UR1: GE1_SNAPSHOT_SIZE=WxH snapshots at that size (the display game rect,
    // what a display-sized export draws); default 640x480.
    unsigned sw = 640, sh = 480;
    if (const char* ss = std::getenv("GE1_SNAPSHOT_SIZE"); ss && !parse_size(ss, &sw, &sh))
        sw = 640, sh = 480;
    if (!GSSaveSnapshotToMemory(sw, sh, false, false, width, height, &s_pixels))
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

extern "C" GE1_API float ge1_gs_back_ms(void)
{
    return s_open ? GSGetAndResetBackThreadMs() : -1.0f;
}

extern "C" GE1_API int ge1_gs_flush_caches(void)
{
    if (!s_open)
        return 0;
    GSFlushPipelineCache();
    persist_recorded_selectors();
    return 1;
}

extern "C" GE1_API int ge1_gs_freeze_size(void)
{
    if (!s_open)
        return 0;
    freezeData fd{0, nullptr};
    if (GSfreeze(FreezeAction::Size, &fd) != 0 || fd.size <= 0)
        return 0;
    return fd.size;
}

extern "C" GE1_API int ge1_gs_freeze_save(uint8_t* out, uint32_t size)
{
    if (!s_open || !out || size == 0)
        return 0;
    // Read render targets back into VRAM for this save only, so the frozen
    // VRAM is complete; the flag is restored before returning.
    const bool read_tc = GSConfig.UserHacks_ReadTCOnClose;
    GSConfig.UserHacks_ReadTCOnClose = true;
    freezeData fd{static_cast<int>(size), out};
    const int rc = GSfreeze(FreezeAction::Save, &fd);
    GSConfig.UserHacks_ReadTCOnClose = read_tc;
    return rc == 0 ? 1 : 0;
}

extern "C" GE1_API int ge1_gs_freeze_load(const uint8_t* data, uint32_t size)
{
    if (!s_open || !data || size == 0)
        return 0;
    freezeData fd{static_cast<int>(size), const_cast<u8*>(data)};
    return GSfreeze(FreezeAction::Load, &fd) == 0 ? 1 : 0;
}
