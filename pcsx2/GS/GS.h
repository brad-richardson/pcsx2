// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <functional>

#include "common/WindowInfo.h"
#include "SaveState.h"
#include "pcsx2/Config.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class RenderAPI
{
	None,
	D3D11,
	Metal,
	D3D12,
	Vulkan,
	OpenGL
};

enum class GSVideoMode : u8
{
	Unknown,
	NTSC,
	PAL,
	VESA,
	SDTV_480P,
	HDTV_720P,
	HDTV_1080I
};

enum class GSDisplayAlignment
{
	Center,
	LeftOrTop,
	RightOrBottom
};

struct GSAdapterInfo
{
	std::string name;
	std::vector<std::string> fullscreen_modes;
	u32 max_texture_size;
	u32 max_upscale_multiplier;
};

class SmallStringBase;

// Returns the ID for the specified function, otherwise -1.
s16 GSLookupGetSkipCountFunctionId(const std::string_view name);
s16 GSLookupBeforeDrawFunctionId(const std::string_view name);
s16 GSLookupMoveHandlerFunctionId(const std::string_view name);

/// Deletes every GS shader and pipeline cache file. While the GS thread runs the work is posted
/// there, where the open caches can be emptied and nothing is written back afterwards, so call it
/// from the CPU thread. `done` receives the number of files removed or emptied, on the thread that
/// did the work.
void GSClearShaderCache(std::function<void(u32)> done = {});
/// The same, for a caller already on the GS thread (the fullscreen UI), or when no GS thread runs.
u32 GSClearShaderCacheOnGSThread();

bool GSopen(const Pcsx2Config::GSOptions& config, GSRendererType renderer, u8* basemem,
	GSVSyncMode vsync_mode, bool allow_present_throttle);
bool GSreopen(bool recreate_device, bool recreate_renderer, GSRendererType new_renderer,
	std::optional<const Pcsx2Config::GSOptions*> old_config);
void GSreset(bool hardware_reset);
void GSclose();
void GSgifSoftReset(u32 mask);
void GSwriteCSR(u32 csr);
void GSInitAndReadFIFO(u8* mem, u32 size);
// LT1b: ticketed asynchronous local->host probes (PS2X lagF). Request at the probe's
// stream point (right after its TRXDIR=1 packet); resolve at VSync for tickets whose
// records have executed (the VSync drain guarantees it for earlier requests); take
// returns the bytes once (0 = not resolved). Stats: see GSState::m_probe_stats.
#define GS_HAS_PROBE_API 1
void GSProbeRequest(u64 bitbltbuf, u64 trxpos, u64 trxreg, u64 ticket);
u32 GSProbeResolve(u64 ticket_lo, u64 ticket_hi);
u32 GSProbeTake(u64 ticket, u8* out, u32 bytes);
void GSProbeStats(u64 out[8]);
void GSReadLocalMemoryUnsync(u8* mem, u32 qwc, u64 BITBLITBUF, u64 TRXPOS, u64 TRXREG);
void GSgifTransfer(const u8* mem, u32 size);
// NRS1: one compact native record (GSCompactRecord.h). PATH1 vertex packets
// with the GIF register padding stripped. Returns false (no state changed)
// on a malformed record; the caller then falls back to GIF packets.
bool GSgifTransferCompact(const u8* bytes, u32 size);
// RZV1 S4b: called by TransferCompact for every packet after its PRIM is
// applied, with the kick's per-packet parse parameters (latched UV, depth
// clamp mode as GSLimit24BitDepth); null = off.
using GSCompactPacketHook = void (*)(u32 packet, u32 uv, int depth_clamp_mode);
void GSSetCompactPacketHook(GSCompactPacketHook hook);
// RZV1 S4c: the static-world packets of a compact record prepared on the
// caller's thread (the MTVU) against the cull state GsWorker last published:
// writes an S4C1 block (GSStaticPrep.h) to out and returns its size, 0 when
// nothing is published yet or cap is too small. Needs IEEE round-to-nearest,
// no flush-to-zero (the trace's S/Q divides). Thread-safe.
u32 GSStaticPrepareRecord(const u8* compact, u32 size, u8* out, u32 cap);
// GsWorker: the next GSgifTransferCompact's packets use this block's prepared
// outcomes where their cull state is the live one (validated; ignored if
// malformed). End clears it.
void GSStaticRecordBegin(const u8* block, u32 size);
void GSStaticRecordEnd();
void GSgifTransfer1(u8* mem, u32 addr);
void GSgifTransfer2(u8* mem, u32 size);
void GSgifTransfer3(u8* mem, u32 size);
void GSvsync(u32 field, bool registers_written);
// Manual frameskip (Android low-end devices): present 1 of every (frames+1)
// VSyncs, skipping presentation of the rest. 0 disables. See GSRenderer::VSync.
void GSSetManualFrameSkip(u32 frames);
u32 GSGetManualFrameSkip();
// Max presented-FPS cap. Caps the DISPLAY frame rate without touching emulation
// speed — dropped on the GS thread in GSRenderer::VSync. 0 disables.
void GSSetMaxPresentFps(u32 fps, u64 present_interval, u32 milli_fps = 0);
u32 GSGetMaxPresentFps();
u32 GSGetMaxPresentMilliFps();
u64 GSGetMaxPresentInterval();
// Allows capped hardware-renderer frames to bypass final display composition and
// post-processing when no correctness-sensitive consumer needs the finished image.
// Platform opt-in keeps the existing presentation-only behavior as the default.
void GSSetPresentCapRenderSkip(bool enabled);
bool GSGetPresentCapRenderSkip();
// While true (set when the limiter enters Turbo / fast-forward), the present cap
// above is bypassed so the speed-up is actually visible. The cap resumes — with
// a clean re-prime, no catch-up burst — as soon as fast-forward ends.
void GSSetPresentCapSuspended(bool suspended);
bool GSGetPresentCapSuspended();
int GSfreeze(FreezeAction mode, freezeData* data);
std::string GSGetBaseSnapshotFilename();
// False if there is no renderer, or a snapshot is already queued and this request was dropped.
bool GSQueueSnapshot(const std::string& path, u32 gsdump_frames = 0);
// True while a dump is open and taking frames. Queueing a snapshot over one of these does not
// start a second dump -- it overwrites the remaining frame count and cuts the recording short.
bool GSIsDumpRecording();
// True when the two-object split is live: a pipelined back thread with its own front parser.
// Not the same question as the BackThreadMode setting, which downgrades to lockstep when the
// split is unsupported, so this is the only way to tell whether the mode really engaged.
bool GSHasFrontParser();
// Waits until the GS back thread has executed every queued record. MTGS thread only. No-op with
// GS multi-threading off.
void GSDrainBackQueue();
void GSStopGSDump();
void GSPresentCurrentFrame();
void GSThrottlePresentation();
void GSGameChanged(const std::string& serial, u32 crc);
void GSSetDisplayAlignment(GSDisplayAlignment alignment);
void GSSetPortraitRenderTopAlign(bool enabled);
/// Pixels kept clear at the top of a portrait window (display cutout / camera).
void GSSetPortraitRenderTopInset(int pixels);
/// Top-align the render in a LANDSCAPE window (foldables / clamshell controllers).
void GSSetLandscapeRenderTopAlign(bool enabled);
bool GSHasDisplayWindow();
void GSResizeDisplayWindow(u32 width, u32 height, float scale);
void GSUpdateDisplayWindow();
void GSSetVSyncMode(GSVSyncMode mode, bool allow_present_throttle);
// GE1 offline replay timing hook; keeps the adapter on the GS.h surface.
float GSGetAndResetAccumulatedGPUTime();
// PT2: back-thread drain busy ms since the last call (reset on read); <0 when
// no back thread is engaged (Off/InlineRecords modes, GL fallback, closed).
float GSGetAndResetBackThreadMs();
// PW1: pipeline-cache pre-warm. All no-op (zeros) unless the VK device is up; GS thread only.
void GSFlushPipelineCache();
void GSSetPipelineCacheFlushDeferred(bool deferred);
void GSGetAndResetPipelineStats(u64* tfx_pipelines, u64* tfx_ns, u64* spv_compiles, u64* spv_ns);
u32 GSGetTFXSelectorSize();
void GSSetTFXSelectorRecord(bool enabled);
u32 GSTakeRecordedTFXSelectors(void* out, u32 capacity);
u32 GSPrewarmTFXPipelines(const void* selectors, u32 count);
// SH1: stall tags since the last call (reset on read): slow TFX creates (>= 1 ms), slowest create ns,
// texture upload bytes, texture uploads, new textures, new-texture ns. Zeros unless the VK device is up.
void GSGetAndResetStallStats(u64 out[6]);

GSRendererType GSGetCurrentRenderer();
bool GSIsHardwareRenderer();

/// Whether this renderer's GetOutput() reads the framebuffer at the display's own offset,
/// block-aligning as it goes, rather than reading the whole buffer and leaving the offset
/// for the presenter. Today this is exactly "is the software renderer", but it is a property
/// of the output path rather than of the renderer kind, and it is resolved per renderer
/// instance so it stays that way. Ask this — never GSIsHardwareRenderer() — when the
/// question is about presentation geometry.
bool GSPresenterOffsetsFramebufferRead();
std::string GetDefaultAdapter();
bool GSWantsExclusiveFullscreen();
std::optional<float> GSGetHostRefreshRate();
std::vector<GSAdapterInfo> GSGetAdapterInfo(GSRendererType renderer);
u32 GSGetMaxUpscaleMultiplier(u32 max_texture_size);
GSVideoMode GSgetDisplayMode();
void GSgetInternalResolution(int* width, int* height);
void GSgetStats(SmallStringBase& info);
void GSgetMemoryStats(SmallStringBase& info);
void GSgetTitleStats(std::string& info);

/// Converts window position to normalized display coordinates (0..1). A value less than 0 or greater than 1 is
/// returned if the position lies outside the display area.
void GSTranslateWindowToDisplayCoordinates(float window_x, float window_y, float* display_x, float* display_y);
void GSTranslateWindowToDisplayCoordinatesUnclamped(float window_x, float window_y, float* display_x, float* display_y);
void GSTranslateDisplayToWindowCoordinates(float display_x, float display_y, float* window_x, float* window_y);

void GSUpdateConfig(const Pcsx2Config::GSOptions& new_config);
void GSSetSoftwareRendering(bool software_renderer, GSInterlaceMode new_interlace);
bool GSSaveSnapshotToMemory(u32 window_width, u32 window_height, bool apply_aspect, bool crop_borders,
	u32* width, u32* height, std::vector<u32>* pixels);
#ifdef __ANDROID__
struct AHardwareBuffer;
// GE1 Android platform seam: copy the composed GS image into an imported AHB.
// 1=exported, 0=no composed frame yet, -1=export failed.
int GSExportSnapshotToAHB(AHardwareBuffer* buffer, u32 width, u32 height, u64* fence_counter);
void GSWaitExportFence(u64 fence_counter);
void GSReleaseExportAHB(AHardwareBuffer* buffer);
// HUD4: composite one Tricky HUD scene onto an already-exported AHB, on this
// device's queue, right behind the export copy (same queue, in order). The
// scene blob is the runtime's Ge1HudScene (magic + version + fixed-size
// geometry; re-validated here, fail-closed); the atlas is w*h*4 RGBA bytes
// uploaded once per (id, w, h). 1=composited (*fence_counter covers the
// export too), 0=unsupported/unready, -1=composite failed (CPU fallback).
#define GS_HAS_HUD_SCENE_API 1
int GSCompositeHudAHB(AHardwareBuffer* buffer, const void* scene, u32 sceneSize, const u8* atlasPx,
	u32 atlasW, u32 atlasH, u64 atlasId, u64* fence_counter);
#endif
#ifdef __APPLE__
// GI1 iOS platform seam: GPU copy of the composed GS image into an
// IOSurface-backed sink (BGRA). 1=export queued (done(ctx, ok) then fires
// exactly once from the command buffer's completion handler, on an arbitrary
// Metal thread; the surface must stay alive until then), 0=no composed frame
// yet (no callback), -1=export failed (no callback).
typedef void (*GSExportIOSurfaceDoneFn)(void* ctx, int ok);
int GSExportSnapshotToIOSurface(void* iosurface, u32 width, u32 height, GSExportIOSurfaceDoneFn done, void* ctx);
void GSReleaseExportIOSurface(void* iosurface);
#endif
void GSJoinSnapshotThreads();

namespace Host
{
	/// Called when the GS is creating a render device.
	/// This could also be fullscreen transition.
	std::optional<WindowInfo> AcquireRenderWindow(bool recreate_window);

	/// Called before drawing the OSD and other display elements.
	void BeginPresentFrame();

	/// Called when the GS is finished with a render window.
	void ReleaseRenderWindow();

	/// Returns true if the hosting application is currently fullscreen.
	bool IsFullscreen();

	/// Alters fullscreen state of hosting application.
	void SetFullscreen(bool enabled);

}

extern Pcsx2Config::GSOptions GSConfig;
