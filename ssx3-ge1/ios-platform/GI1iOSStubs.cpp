// GI1: iOS stubs for platform pieces the embedded SDKs lack. None of this runs
// on the iOS GS path.
//
// CN1A (ARMSX2 base): the optical drive, HTTPDownloader::Create and
// Common::PlaySoundAsync stubs, plus the iOS app-bridge hooks the core calls
// under TARGET_OS_IPHONE (ARMSX2_iOS*, ARMSX2_PostEmulationOnlyStartupReady),
// now come from ARMSX2's own pcsx2/Apple/AppleEmbeddedStubs.cpp, which
// ios-platform/CMakeLists.txt compiles; only the Discord stubs stay here.

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
