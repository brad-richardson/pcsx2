// GI1: CocoaTools subset for iOS (CocoaTools.mm is AppKit/macOS-only). Same
// signatures, NSBundle-backed; no translocation or refresh-rate queries exist
// on iOS. Everything here is reachable on the GS path (SetAppRoot,
// SetResourcesDirectory, Frameworks dlopen search, refresh-rate query).
#ifdef __APPLE__

#include "common/CocoaTools.h"

#import <Foundation/Foundation.h>

std::optional<std::string> CocoaTools::GetBundlePath()
{
	std::optional<std::string> ret;
	@autoreleasepool {
		NSURL* url = [NSURL fileURLWithPath:[[NSBundle mainBundle] bundlePath]];
		if (url)
			ret = std::string([url fileSystemRepresentation]);
	}
	return ret;
}

std::optional<std::string> CocoaTools::GetNonTranslocatedBundlePath()
{
	// iOS apps are never translocated; the bundle path is the answer.
	return GetBundlePath();
}

std::optional<std::string> CocoaTools::GetResourcePath()
{
	@autoreleasepool {
		if (NSBundle* bundle = [NSBundle mainBundle])
		{
			// Flat bundle: resourcePath == bundlePath on iOS. (A "resources"
			// subdir would break bundle signing, so resources live at top
			// level: fonts/, default.metallib.)
			return [[bundle resourcePath] UTF8String];
		}
		return std::nullopt;
	}
}

std::optional<float> CocoaTools::GetViewRefreshRate(const WindowInfo& wi)
{
	(void)wi;
	return std::nullopt;
}

#endif
