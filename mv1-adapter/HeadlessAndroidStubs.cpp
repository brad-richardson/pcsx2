// MV1 adb-shell bench: JNI-only platform services are unreachable.
#include "common/FileSystem.h"
#include "common/HostSys.h"
#include "Input/AndroidNativeRumble.h"

int FileSystem::OpenFDFileContent(const char*) { return -1; }
bool FileSystem::CreateDirectoryViaJava(const char*) { return false; }
bool FileSystem::CreateFileViaJava(const char*) { return false; }
bool Common::PlaySoundAsync(const char*) { return false; }
void Native::onPadRumble(int, int, int) {}
