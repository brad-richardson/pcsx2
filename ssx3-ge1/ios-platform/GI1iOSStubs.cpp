// GI1: iOS stubs for platform pieces the embedded SDKs lack. None of this runs
// on the iOS GS path; the shapes follow ARMSX2's AppleEmbeddedStubs.cpp:
//  - optical drive: iOS devices have no disc drive (empty list, failed reads);
//  - HTTPDownloader::Create: no libcurl; callers already treat null as
//    "unavailable" (achievements, cover downloads);
//  - Common::PlaySoundAsync: lives in CocoaTools.mm (AppKit, macOS-only);
//    the achievement chime treats false as "no sound played".
#include "CDVD/CDVDdiscReader.h"
#include "DEV9/net.h"

#include "common/HostSys.h"
#include "common/HTTPDownloader.h"

#include <memory>
#include <string>
#include <vector>

std::unique_ptr<HTTPDownloader> HTTPDownloader::Create(std::string user_agent)
{
	(void)user_agent;
	return nullptr;
}

bool Common::PlaySoundAsync(const char* path)
{
	(void)path;
	return false;
}

// Discord game registration is desktop-only (custom URL schemes / Steam AppID
// files); the client calls it from Discord_Initialize, which nothing on the
// iOS GS path reaches, but the rpc object pulls the references regardless.
extern "C" {
void Discord_Register(const char* applicationId, const char* command)
{
	(void)applicationId;
	(void)command;
}
void Discord_RegisterSteamGame(const char* applicationId, const char* steamId)
{
	(void)applicationId;
	(void)steamId;
}
}

std::vector<std::string> GetOpticalDriveList()
{
	return {};
}

void GetValidDrive(std::string& drive)
{
	drive.clear();
}

IOCtlSrc::IOCtlSrc(std::string filename)
	: m_filename(std::move(filename))
{
}

IOCtlSrc::~IOCtlSrc()
{
}

bool IOCtlSrc::Reopen(Error* error)
{
	(void)error;
	return false;
}

bool IOCtlSrc::DiscReady()
{
	return false;
}

u32 IOCtlSrc::GetSectorCount() const
{
	return 0;
}

s32 IOCtlSrc::GetMediaType() const
{
	return 0;
}

const std::vector<toc_entry>& IOCtlSrc::ReadTOC() const
{
	static const std::vector<toc_entry> empty;
	return empty;
}

bool IOCtlSrc::ReadSectors2048(u32 sector, u32 count, u8* buffer) const
{
	(void)sector;
	(void)count;
	(void)buffer;
	return false;
}

bool IOCtlSrc::ReadSectors2352(u32 sector, u32 count, u8* buffer) const
{
	(void)sector;
	(void)count;
	(void)buffer;
	return false;
}

bool IOCtlSrc::ReadTrackSubQ(cdvdSubQ* subq) const
{
	(void)subq;
	return false;
}

u32 IOCtlSrc::GetLayerBreakAddress() const
{
	return 0;
}

void IOCtlSrc::SetSpindleSpeed(bool restore_defaults) const
{
	(void)restore_defaults;
}

// PCAPAdapter stubs: on the ARMSX2 base pcsx2/CMakeLists.txt compiles Android/AndroidPcapStubs.cpp
// for ARMSX2_IOS, so they are no longer carried here (CN1A).
