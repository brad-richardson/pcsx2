// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// GP2: minimal ARMSX2 vertex-kick port — the packed STQRGBAXYZF2 parse kernels
// only. Extracted verbatim from ARMSX2's pcsx2/GS/GSVertexKick.h (parse section);
// no layouts, cull grids, FMM fusion, kernel or BackQueue. The kernels are pure
// functions of their arguments: the aarch64 TBL build is bit-identical to the
// portable one (pinned by ARMSX2's gs_vertex_tests), and the portable one is
// op-for-op the sequence GIFPackedRegHandlerSTQRGBAXYZF2 already runs.

#include "GS/GSRegs.h"
#include "GS/GSVector.h"

#include <cfloat>

namespace GSVertexKernels
{
	// Parse one packed {STQ, RGBAQ, XYZF2} record (r[0..2]) into GSVertex m[0]/m[1].
	// uv = the latched UV register value (packed XYZF2 does not write UV). Q == +0.0
	// (integer compare, so -0.0 passes through) is rewritten to FLT_MIN to avoid
	// divides by zero downstream — matches GIFPackedRegHandlerSTQ.
	__forceinline_odr void ParsePackedSTQRGBAXYZF2(const GIFPackedReg* RESTRICT r, u32 uv, GSVector4i& m0, GSVector4i& m1)
	{
		const GSVector4i st = GSVector4i::loadl(&r[0].U64[0]);
		GSVector4i q = GSVector4i::loadl(&r[0].U64[1]);
		const GSVector4i rgba = (GSVector4i::load<false>(&r[1]) & GSVector4i::x000000ff()).ps32().pu16();

		q = q.blend8(GSVector4i::cast(GSVector4(FLT_MIN)), q == GSVector4i::zero());

		m0 = st.upl64(rgba.upl32(q));

		GSVector4i xy = GSVector4i::loadl(&r[2].U64[0]);
		GSVector4i zf = GSVector4i::loadl(&r[2].U64[1]);
		xy = xy.upl16(xy.srl<4>()).upl32(GSVector4i::load((int)uv));
		zf = zf.srl32<4>() & GSVector4i::x00ffffff().upl32(GSVector4i::x000000ff());

		m1 = xy.upl32(zf);
	}

#ifdef ARCH_ARM64
	// aarch64-native parse: the whole vertex build is byte movement, so one TBL
	// gathers each qword (the legacy path spends ~11 NEON ops on the RGBA
	// pack chain alone). Out-of-range TBL indices read as zero, which provides the
	// 24-bit Z and 8-bit F masks for free. Bit-identical to the legacy kernel.
	//
	// The TBL patterns and the Q fix-up constant are lifted into a struct a caller
	// can hoist out of a batch loop.
	struct PackedParseConsts
	{
		uint8x16_t pat_m0;   // {S, T, RGBA, Q}, shared by both layouts
		uint8x16_t pat_m1;   // XYZF2: {X|Y<<16, Z, -, F}
		uint8x16_t pat_xyz;  // XYZ2:  {X|Y<<16, Z32} into the low half (kept for the struct's shape)
		uint32x4_t q_fixup;  // Q == +0.0 rewrites to FLT_MIN
	};

	__forceinline_odr PackedParseConsts MakePackedParseConsts()
	{
		alignas(16) static constexpr u8 pat_m0[16] = {0, 1, 2, 3, 4, 5, 6, 7, 16, 20, 24, 28, 8, 9, 10, 11};
		alignas(16) static constexpr u8 pat_m1[16] = {0, 1, 4, 5, 24, 25, 26, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 28, 0xFF, 0xFF, 0xFF};
		alignas(16) static constexpr u8 pat_xyz[16] = {0, 1, 4, 5, 8, 9, 10, 11, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
		alignas(16) static constexpr u32 q_fixup[4] = {0, 0, 0, 0x00800000};

		PackedParseConsts k;
		k.pat_m0 = vld1q_u8(pat_m0);
		k.pat_m1 = vld1q_u8(pat_m1);
		k.pat_xyz = vld1q_u8(pat_xyz);
		k.q_fixup = vld1q_u32(q_fixup);
		return k;
	}

	__forceinline_odr void ParsePackedSTQRGBAXYZF2_Neon(const GIFPackedReg* RESTRICT r, u32 uv,
		const PackedParseConsts& k, GSVector4i& m0, GSVector4i& m1)
	{
		const uint8x16x2_t st_rgba = {vld1q_u8(reinterpret_cast<const u8*>(r + 0)), vld1q_u8(reinterpret_cast<const u8*>(r + 1))};
		uint32x4_t v0 = vreinterpretq_u32_u8(vqtbl2q_u8(st_rgba, k.pat_m0));
		v0 = vorrq_u32(v0, vandq_u32(vceqzq_u32(v0), k.q_fixup));

		const uint8x16_t xyzf = vld1q_u8(reinterpret_cast<const u8*>(r + 2));
		const uint8x16x2_t xyzf_pair = {xyzf, vreinterpretq_u8_u32(vshrq_n_u32(vreinterpretq_u32_u8(xyzf), 4))};
		uint32x4_t v1 = vreinterpretq_u32_u8(vqtbl2q_u8(xyzf_pair, k.pat_m1));
		v1 = vsetq_lane_u32(uv, v1, 2);

		m0 = GSVector4i(vreinterpretq_s32_u32(v0));
		m1 = GSVector4i(vreinterpretq_s32_u32(v1));
	}

	__forceinline_odr void ParsePackedSTQRGBAXYZF2_Neon(const GIFPackedReg* RESTRICT r, u32 uv, GSVector4i& m0, GSVector4i& m1)
	{
		ParsePackedSTQRGBAXYZF2_Neon(r, uv, MakePackedParseConsts(), m0, m1);
	}

	// Hoisted-consts entry for batch loops: the caller builds the consts once.
	__forceinline_odr void ParsePackedSTQRGBAXYZF2_Fast(const GIFPackedReg* RESTRICT r, u32 uv,
		const PackedParseConsts& k, GSVector4i& m0, GSVector4i& m1)
	{
		ParsePackedSTQRGBAXYZF2_Neon(r, uv, k, m0, m1);
	}
#endif // ARCH_ARM64

	// Dispatcher the fused handler calls: aarch64 takes the TBL kernel, x86
	// keeps the legacy path.
	__forceinline_odr void ParsePackedSTQRGBAXYZF2_Fast(const GIFPackedReg* RESTRICT r, u32 uv, GSVector4i& m0, GSVector4i& m1)
	{
#ifdef ARCH_ARM64
		ParsePackedSTQRGBAXYZF2_Neon(r, uv, m0, m1);
#else
		ParsePackedSTQRGBAXYZF2(r, uv, m0, m1);
#endif
	}
} // namespace GSVertexKernels
