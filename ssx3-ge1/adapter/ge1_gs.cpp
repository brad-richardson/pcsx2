// GE1's private PCSX2 GS host. This adapter calls public GS.h entry points.
#include "ge1_gs.h"
#include "pcsx2/GS.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
alignas(16) std::array<u8, 8192> s_priv{};
std::vector<u32> s_pixels;
MemorySettingsInterface s_settings;
bool s_open = false;
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
    config.Renderer = GSRendererType::VK;
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
    std::fill(s_priv.begin(), s_priv.end(), 0);
    s_open = GSopen(config, GSRendererType::VK, s_priv.data(), GSVSyncMode::Disabled, false);
    return s_open ? 1 : 0;
}

extern "C" GE1_API void ge1_gs_close(void)
{
    if (s_open)
        GSclose();
    s_open = false;
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

extern "C" GE1_API float ge1_gs_gpu_ms(void)
{
    return s_open ? GSGetAndResetAccumulatedGPUTime() : -1.0f;
}
