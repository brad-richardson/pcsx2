// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSRegs.h"
#include "GS/GSVector.h"
#include "GS/Renderers/HW/GSVertexHW.h"
#include "GS/Renderers/SW/GSVertexSW.h"

struct alignas(32) GSVertex
{
	union
	{
		struct
		{
			GIFRegST ST;       // S:0, T:4
			GIFRegRGBAQ RGBAQ; // RGBA:8, Q:12
			GIFRegXYZ XYZ;     // XY:16, Z:20
			union { u32 UV; struct { u16 U, V; }; }; // UV:24
			u32 FOG;        // FOG:28
		};

#if defined(ARCH_X86)
#if _M_SSE >= 0x500
		__m256i mx;
#endif
		__m128i m[2];
#elif defined(ARCH_ARM64)
		int32x4_t m[2];
#endif
	};
};

static_assert(sizeof(GSVertex) == 32);

// TPF1 terrain pass-fusion vertex: one ST pair per source pass (ST is the only
// vertex field the template allows to differ) plus the shared RGBAQ/XYZ/UV/FOG
// (byte-identical across the triple by VertexStreamsMatch). Consumed only by
// VSExpand::Fuse3 shaders, which read it from the vertex storage buffer.
struct GSVertexFuse3
{
	GIFRegST ST[3];      // bytes 0..24: S0/T0, S1/T1, S2/T2 (floats)
	GIFRegRGBAQ RGBAQ;   // bytes 24..32: RGBA:24, Q:28
	GIFRegXYZ XYZ;       // bytes 32..40: XY:32, Z:36
	union { u32 UV; struct { u16 U, V; }; }; // UV:40
	u32 FOG;             // FOG:44
};

static_assert(sizeof(GSVertexFuse3) == 48);
static_assert(offsetof(GSVertexFuse3, RGBAQ) == 24);
static_assert(offsetof(GSVertexFuse3, XYZ) == 32);

struct alignas(32) GSVertexPT1
{
	GSVector4 p;
	GSVector2 t;
	char pad[4];
	union { u32 c; struct { u8 r, g, b, a; }; };
};

static_assert(sizeof(GSVertexPT1) == sizeof(GSVertex));

__forceinline_odr GSVector4i GetVertexXY(const GSVertex& v)
{
	return GSVector4i(v.m[1]).upl16().xyxy();
}

__forceinline_odr GSVector4i GetVertexZ(const GSVertex& v)
{
	return GSVector4i(v.m[1]).yyyy();
}

__forceinline_odr GSVector4i GetVertexUV(const GSVertex& v)
{
	return GSVector4i(v.m[1]).uph16().xyxy();
}

__forceinline_odr GSVector4 GetVertexST(const GSVertex& v)
{
	return GSVector4::cast(GSVector4i(v.m[0])).xyxy();
}

__forceinline_odr GSVector4i GetVertexRGBA(const GSVertex& v)
{
	return GSVector4i(v.m[0]).uph8().upl16();
}

__forceinline_odr GSVector4 GetVertexQ(const GSVertex& v)
{
	return GSVector4::cast(GSVector4i(v.m[0])).wwww();
}

__forceinline_odr GSVector4i GetVertexFOG(const GSVertex& v)
{
	return GSVector4i(v.m[1]).wwww();
}