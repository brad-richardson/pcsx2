// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GS.h"

#include <string>
#include <vector>

// Header with all metal stuff available for use with C++ (rather than Objective-C++)

#ifdef __APPLE__

class GSDevice;
class GSTexture;
GSDevice* MakeGSDeviceMTL();
std::vector<GSAdapterInfo> GetMetalAdapterList();
// GI1: C++ bridge to GSDeviceMTL's IOSurface export (GSDeviceMTL.h is ObjC++-only).
bool MT_CopySnapshotToIOSurface(GSDevice* dev, GSTexture* source, void* iosurface, u32 width, u32 height,
	u32 pad_x, u32 pad_y, GSExportIOSurfaceDoneFn done, void* ctx);
void MT_ReleaseExportIOSurface(GSDevice* dev, void* iosurface);

#endif
