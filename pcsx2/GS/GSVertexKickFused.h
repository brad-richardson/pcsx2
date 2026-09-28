// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// GP3: the fused vertex-kick path behind GE1_VERTEX_KICK=2 (default off).
//
// The cull grid, scalar-outcode cull and fused vertex-trace bounds below are
// verbatim ARMSX2 (pcsx2/GS/GSVertexKick.h, RV14 tree): pure functions of their
// arguments, pinned by ARMSX2's gs_vertex_tests against an independent scalar
// model. The two-pass kernel is ARMSX2's GSVertexKickKernel.h adapted to our
// upstream-based kick:
//
//   * strip-only (STQRGBAXYZF2 x tristrip is 81% of SP1 kicks; fans keep the
//     per-vertex direct kick in GSState.cpp, everything else the legacy path);
//   * no layout machinery (the contiguous triple only; parse is GP2's
//     GSVertexKickParse.h, already exact);
//   * no kick_ring: chunk seeds are rebuilt from our xy ring with the current
//     cull bounds, so a mid-draw PRIM-class change (which ApplyPRIM allows
//     without a flush) cannot strand stale-class outcodes in the seeds;
//   * no draw-buffering native rect (our config never enables it; the fused
//     path declines when it is on).
//
// Anything here must stay a pure function of its arguments: no GSState
// members, no config reads.

#include "GS/GSRegs.h"
#include "GS/GSVector.h"
#include "GS/GSVertexKickParse.h"
#include "GS/Renderers/Common/GSVertex.h"

#include <algorithm>
#include <cfloat>

namespace GSVertexKernels
{
	// ------------------------------------------------------------------------
	// The cull grid: the sub-texel spacing of the device sample points a prim has
	// to span to paint anything. (Verbatim ARMSX2; see GSVertexKick.h there for
	// the full derivation. Our config is always native res, i.e. shift 4, where
	// the grid IS the pixel centres and CullTest below is op-for-op the upstream
	// VertexKick cull.)
	// ------------------------------------------------------------------------
	struct CullGrid
	{
		GSVector4i round_add;  // (step - 1, step - 1, -1, -1)
		GSVector4i round_mask; // ~(step - 1)
		GSVector4i phase;      // (phase_x, phase_y, phase_x, phase_y)
		int shift;             // triangle class
		int sprite_shift;      // sprite class
		int band_bias_x;       // phase_x + 1, the constant the band expression subtracts
		int band_bias_y;       // phase_y + 1

		template <int primclass>
		__forceinline_odr int ShiftFor() const
		{
			return (primclass == GS_SPRITE_CLASS) ? sprite_shift : shift;
		}
	};

	__forceinline_odr CullGrid MakeCullGrid(int shift, int sprite_shift, int phase_x = 0, int phase_y = 0)
	{
		const int step = 1 << shift;
		return {GSVector4i(step - 1, step - 1, -1, -1), GSVector4i(~(step - 1)),
			GSVector4i(phase_x, phase_y, phase_x, phase_y), shift, sprite_shift, phase_x + 1, phase_y + 1};
	}

	// Raw bounding box of one completed prim's window entries, no rounding.
	template <u32 n>
	__forceinline_odr GSVector4i CullPrimBounds(const GSVector4i& v0, const GSVector4i& v1, const GSVector4i& v2)
	{
		if constexpr (n == 1)
			return v0;
		else if constexpr (n == 2)
			return v0.runion(v1);
		else
		{
			static_assert(n == 3);
			return v0.runion(v1).runion(v2);
		}
	}

	// Snap a bbox inwards onto the grid: top/left up to the first sample point at
	// or past the edge, bottom/right down to the last one strictly inside, then +1
	// on bottom/right so rempty() reads "spans no sample point".
	__forceinline_odr GSVector4i RoundToCullGrid(const GSVector4i& bbox, const CullGrid& grid)
	{
		const GSVector4i interior = (bbox + grid.round_add) & grid.round_mask;
		return interior + GSVector4i(0, 0, 1, 1);
	}

	// The rounded bbox the accepted-prim draw_rect update and the scissor test
	// consume. At shift 4 this is the shipped interior-pixel-centre rounding,
	// op-for-op with the upstream VertexKick native arm.
	template <int primclass>
	__forceinline_odr GSVector4i RoundCullRect(GSVector4i bbox, const CullGrid& grid, bool aa1_expand)
	{
		if constexpr (primclass == GS_TRIANGLE_CLASS || primclass == GS_SPRITE_CLASS)
		{
			if (grid.ShiftFor<primclass>() == 4)
			{
				// Native: the grid IS the pixel centres, so this is the shipped
				// interior-pixel-centre rounding.
				bbox = RoundToCullGrid(bbox, grid);
			}
			else
			{
				// For upscaling, remove bottom/right subtexels.
				bbox -= ((bbox & GSVector4i(0xF)) == GSVector4i(0)) & GSVector4i(0, 0, 1, 1);
			}

			// For AA1 triangles and lines, expand the bounds by 1 pixel on all sides.
			if (aa1_expand)
			{
				bbox += GSVector4i(-0x10, -0x10, 0x10, 0x10);
			}
		}

		return bbox;
	}

	// The pixel rect one accepted prim contributes to temp_draw_rect: the class
	// rounding is already in bbox, this is only the sub-pixel shift and the
	// exclusive bottom/right endpoint.
	__forceinline_odr GSVector4i PrimDrawRect(const GSVector4i& bbox)
	{
		return bbox.sra32<4>() + GSVector4i(0, 0, 1, 1);
	}

	// Whether the prim spans no point of the cull grid, and so paints nothing.
	// Only asked where the grid is finer than the rect rounding, i.e. shift 1..3:
	// at shift 4 the rect IS the grid and its own rempty() already says this.
	template <int primclass>
	__forceinline_odr u32 CullGridEmpty(const GSVector4i& bbox, const CullGrid& grid, bool aa1_expand)
	{
		const int shift = grid.ShiftFor<primclass>();
		if (shift == 0 || shift == 4)
			return 0;

		GSVector4i snapped = RoundToCullGrid(bbox - grid.phase, grid);
		if (aa1_expand)
			snapped += GSVector4i(-0x10, -0x10, 0x10, 0x10);

		return static_cast<u32>(snapped.rempty());
	}

	// Bounding box of one completed prim's window entries with the class rounding
	// applied — the bbox half of the legacy CullTest, shared by the scalar-outcode
	// fast path (which only needs it for accepted prims).
	template <u32 n, int primclass>
	__forceinline_odr GSVector4i ComputeCullBBox(const GSVector4i& v0, const GSVector4i& v1, const GSVector4i& v2,
		const CullGrid& grid, bool aa1_expand)
	{
		return RoundCullRect<primclass>(CullPrimBounds<n>(v0, v1, v2), grid, aa1_expand);
	}

	// Accept/cull test for one completed prim. v0/v1/v2 are the window entries for
	// the prim's vertices ({x, y, x, y} offset-subtracted 12.4 fixed-point, v0 most
	// recent), scissor_cull = context scissor in cull form. Returns nonzero to
	// skip the prim; bbox receives the rounded bounding box the accepted-prim
	// draw_rect update consumes. At shift 4 with aa1_expand false this is op-for-op
	// the upstream VertexKick cull.
	template <u32 n, int primclass>
	__forceinline_odr u32 CullTest(const GSVector4i& v0, const GSVector4i& v1, const GSVector4i& v2,
		const GSVector4i& scissor_cull, const CullGrid& grid, bool aa1_expand, GSVector4i& bbox)
	{
		const GSVector4i raw = CullPrimBounds<n>(v0, v1, v2);
		bbox = RoundCullRect<primclass>(raw, grid, aa1_expand);

		// Do scissor test.
		const GSVector4i bbox_ex = bbox + GSVector4i(0, 0, 1, 1); // Exclusive coords for the scissor test.
		u32 test = static_cast<u32>(!bbox_ex.rintersects(scissor_cull));

		// Test for empty bbox, and for spanning no point of the cull grid.
		if constexpr (primclass == GS_TRIANGLE_CLASS || primclass == GS_SPRITE_CLASS)
		{
			test |= static_cast<u32>(bbox.rempty());
			test |= CullGridEmpty<primclass>(raw, grid, aa1_expand);
		}

		// Test for degenerate triangle.
		if constexpr (primclass == GS_TRIANGLE_CLASS)
		{
			test |= static_cast<u32>(v0.eq(v1)) | static_cast<u32>(v1.eq(v2)) | static_cast<u32>(v0.eq(v2));
		}

		return test;
	}

	// ------------------------------------------------------------------------
	// Scalar-outcode cull: exact scalar reformulation of CullTest for the cases
	// the fused handlers hit hottest. (Verbatim ARMSX2; derivation in
	// GSVertexKick.h there, pinned by gs_vertex_tests.)
	//
	// Window coords are offset-subtracted s32, so bands are stored as 28-bit
	// fields — exact for any s32 coord, no truncation aliasing.
	// ------------------------------------------------------------------------

	constexpr u64 kCullMetaBandXMask = 0xFFFFFFFull;
	constexpr u64 kCullMetaBandYMask = 0xFFFFFFFull << 28;
	constexpr u64 kCullMetaOutcodeMask = 0xFull << 56;

	// One mirror entry: packed window position + derived cull metadata.
	// xyp = (u64)(u32)wy << 32 | (u32)wx; meta = bandx:28 | bandy:28 | outcode:4.
	struct CullMirrorEntry
	{
		u64 xyp;
		u64 meta;
	};

	// Pre-adjusted per-class scissor bounds, derived from scissor.cull whenever the
	// scissor changes. Outcode bits: 1 = out-left (< l), 2 = out-right (>= r),
	// 4 = out-top (< t), 8 = out-bottom (>= b).
	struct CullBounds
	{
		int l, t, r, b;
	};

	// Band-space bounds for triangle/sprite at native res (rounding folded in).
	__forceinline_odr CullBounds MakeBandedCullBounds(const GSVector4i& cull)
	{
		return {(cull.x + 14) >> 4, (cull.y + 14) >> 4, (cull.z - 1) >> 4, (cull.w - 1) >> 4};
	}

	// Build one mirror entry from a vertex's window position.
	template <bool banded>
	__forceinline_odr CullMirrorEntry MakeCullMirrorEntry(
		int wx, int wy, const CullBounds& bounds, int band_shift, int band_bias_x = 1, int band_bias_y = 1)
	{
		const int bx = (wx - band_bias_x) >> band_shift;
		const int by = (wy - band_bias_y) >> band_shift;
		const int cx = banded ? bx : wx;
		const int cy = banded ? by : wy;

		u32 oc = 0;
		oc |= (cx < bounds.l) ? 1u : 0u;
		oc |= (cx >= bounds.r) ? 2u : 0u;
		oc |= (cy < bounds.t) ? 4u : 0u;
		oc |= (cy >= bounds.b) ? 8u : 0u;

		CullMirrorEntry e;
		e.xyp = static_cast<u64>(static_cast<u32>(wx)) | (static_cast<u64>(static_cast<u32>(wy)) << 32);
		e.meta = (static_cast<u64>(static_cast<u32>(bx)) & kCullMetaBandXMask) |
		         ((static_cast<u64>(static_cast<u32>(by)) << 28) & kCullMetaBandYMask) |
		         (static_cast<u64>(oc) << 56);
		return e;
	}

	// The scalar decision. e0 is the most recent vertex. Bit-equivalent to
	// CullTest's return under the fast-path gate (triangle non-fan class with a
	// cull grid and no AA1 expansion).
	template <u32 n, int primclass>
	__forceinline_odr u32 CullTestScalar(const CullMirrorEntry& e0, const CullMirrorEntry& e1, const CullMirrorEntry& e2)
	{
		u64 all_out = e0.meta;
		if constexpr (n >= 2)
			all_out &= e1.meta;
		if constexpr (n == 3)
			all_out &= e2.meta;

		if ((all_out & kCullMetaOutcodeMask) != 0)
			return 1;

		if constexpr (primclass == GS_TRIANGLE_CLASS || primclass == GS_SPRITE_CLASS)
		{
			// Interior-empty: all vertices in one pixel band on either axis.
			u64 diff = e0.meta ^ e1.meta;
			if constexpr (n == 3)
				diff |= e0.meta ^ e2.meta;

			if ((diff & kCullMetaBandXMask) == 0 || (diff & kCullMetaBandYMask) == 0)
				return 1;
		}

		if constexpr (primclass == GS_TRIANGLE_CLASS)
		{
			if (e0.xyp == e1.xyp || e1.xyp == e2.xyp || e0.xyp == e2.xyp)
				return 1;
		}

		return 0;
	}

	// ------------------------------------------------------------------------
	// Fused vertex-trace bounds: accumulate GSVertexTraceFMM::FindMinMax's
	// min/max at index-emission time over each newly-referenced vertex. (Verbatim
	// ARMSX2; exactness notes in GSVertexKick.h there, pinned by gs_vertex_tests.
	// Our GSVertexTraceFMM.cpp is byte-identical to ARMSX2's, so the equivalence
	// transfers directly.)
	// ------------------------------------------------------------------------

	struct FmmAcc
	{
		GSVector4i pmin, pmax; // u32 min/max of {x, y, z, fog-word} (the legacy p vectors)
		GSVector4i tmin, tmax; // FST: u16 min/max of raw m[1] (elements 4/5 = U/V).
		                       // !FST: the legacy NaN-blend-masked float min/max chains
		                       //       over per-vertex {S/Q, T/Q, Q, Q}.
		GSVector4i tnan;       // !FST: accumulated per-lane NaN masks (legacy tnan)
		GSVector4i cmin, cmax; // u8 min/max of m[0] (bytes 8-11 = RGBA); flat shading
		                       // accumulates provoking vertices only.
	};

	// {x, y, z, fog-word} exactly as the legacy kernel builds its p vectors.
	__forceinline_odr GSVector4i FmmPos(const GSVector4i& m1)
	{
		return m1.upl16().blend32<0xc>(m1.ywyw());
	}

	__forceinline_odr void FmmAccReset(FmmAcc& a, bool tme, bool fst)
	{
		a.pmin = GSVector4i::xffffffff();
		a.pmax = GSVector4i::zero();
		if (tme && !fst)
		{
			a.tmin = GSVector4i::cast(GSVector4(FLT_MAX));
			a.tmax = GSVector4i::cast(GSVector4(-FLT_MAX));
		}
		else
		{
			a.tmin = GSVector4i::xffffffff();
			a.tmax = GSVector4i::zero();
		}
		a.tnan = GSVector4i::zero();
		a.cmin = GSVector4i::xffffffff();
		a.cmax = GSVector4i::zero();
	}

	// accumulate_color = iip || provoking, evaluated by the caller (flat shading
	// only takes the provoking vertex's color; the provoking vertex is always the
	// last-emitted index of the prim).
	__forceinline_odr void FmmAccumVertex(FmmAcc& a, const GSVector4i& m0, const GSVector4i& m1,
		bool tme, bool fst, bool accumulate_color)
	{
		const GSVector4i p = FmmPos(m1);
		a.pmin = a.pmin.min_u32(p);
		a.pmax = a.pmax.max_u32(p);

		if (tme)
		{
			if (fst)
			{
				a.tmin = a.tmin.min_u16(m1);
				a.tmax = a.tmax.max_u16(m1);
			}
			else
			{
				// Single-vertex transcription of the legacy STQ step: build
				// {S/Q, T/Q, Q, Q}, mask NaN lanes out of the min/max chains,
				// record them in tnan.
				const GSVector4 stq_raw = GSVector4::cast(m0);
				const GSVector4 stq = (stq_raw / stq_raw.wwww()).xyww(stq_raw);

				const GSVector4i nan = GSVector4i::cast(stq != stq);
				const GSVector4 keep = GSVector4::cast(~nan);

				GSVector4 tmin = GSVector4::cast(a.tmin);
				GSVector4 tmax = GSVector4::cast(a.tmax);
				a.tmin = GSVector4i::cast(tmin.blend32(tmin.min(stq), keep));
				a.tmax = GSVector4i::cast(tmax.blend32(tmax.max(stq), keep));
				a.tnan |= nan;
			}
		}

		if (accumulate_color)
		{
			a.cmin = a.cmin.min_u8(m0);
			a.cmax = a.cmax.max_u8(m0);
		}
	}

	struct FmmResult
	{
		GSVector4 min_p, max_p, min_t, max_t;
		GSVector4i min_c, max_c;
		u32 nan_value;  // only meaningful when write_nan
		bool write_nan; // legacy leaves vt.nan untouched for TME && FST draws
	};

	// Reproduce the legacy FindMinMax tail from the accumulators. tw/th are the
	// draw context's TEX0.TW/TH.
	__forceinline_odr void FmmFinish(const FmmAcc& a, bool tme, bool fst, bool color,
		const GIFRegXYOFFSET& ofs, u32 tw, u32 th, FmmResult& out)
	{
		out.write_nan = !(tme && fst);
		out.nan_value = 0;

		const GSVector4 o(ofs);
		const GSVector4 s(1.0f / 16, 1.0f / 16, 2.0f, 1.0f);

		out.min_p = (GSVector4(a.pmin) - o) * s;
		out.max_p = (GSVector4(a.pmax) - o) * s;

		// Fix signed int conversion of the Z lane, as the legacy tail does.
		out.min_p = out.min_p.insert32<0, 2>(GSVector4::load(static_cast<float>(static_cast<u32>(a.pmin.extract32<2>()))));
		out.max_p = out.max_p.insert32<0, 2>(GSVector4::load(static_cast<float>(static_cast<u32>(a.pmax.extract32<2>()))));

		if (tme)
		{
			if (fst)
			{
				// Legacy converts each vertex's {U, V} u16s to float and min/maxes
				// against FLT_MAX sentinels; u16 -> float is monotone and exact and
				// the sentinels never survive, so min-in-u16-then-convert matches.
				const GSVector4i uvmin(a.tmin.U16[4], a.tmin.U16[5], a.tmin.U16[4], a.tmin.U16[5]);
				const GSVector4i uvmax(a.tmax.U16[4], a.tmax.U16[5], a.tmax.U16[4], a.tmax.U16[5]);
				const GSVector4 sc = GSVector4(1.0f / 16, 1.0f).xxyy();
				out.min_t = GSVector4(uvmin) * sc;
				out.max_t = GSVector4(uvmax) * sc;
			}
			else
			{
				const GSVector4 sc = GSVector4(1 << static_cast<int>(tw), 1 << static_cast<int>(th), 1, 1);
				out.min_t = GSVector4::cast(a.tmin) * sc;
				out.max_t = GSVector4::cast(a.tmax) * sc;
				out.nan_value = static_cast<u32>(a.tnan.mask()) & ~4u;
			}
		}
		else
		{
			out.min_t = GSVector4::zero();
			out.max_t = GSVector4::zero();
		}

		if (color)
		{
			out.min_c = a.cmin.zzzz().u8to32();
			out.max_c = a.cmax.zzzz().u8to32();
		}
		else
		{
			out.min_c = GSVector4i::zero();
			out.max_c = GSVector4i::zero();
		}
	}
} // namespace GSVertexKernels

// Two-pass packed-vertex kick kernel for triangle strips.
// Adapted from ARMSX2's GSVertexKickKernel.h (see the file header for the
// adaptation list). The caller (GSState::KickPackedFused) owns every seam; the
// kernel owns the runs between them.
//
// Nothing here calls out to GSState. Anything that needs the rest of it -- a
// flush, a buffer growth, the per-draw environment snapshot -- is the caller's
// business: the driver runs the legacy kick for those vertices and re-enters,
// and the kernel holds nothing across such a seam.
namespace GSVertexKickKernel
{
	// Vertices per kernel entry. The side table is 16 bytes a vertex, so this is
	// a 2 KB working set.
	static constexpr u32 kChunkVertices = 128;

	// Below this many vertices in a handler call, the caller runs its per-vertex
	// direct batch instead of entering the kernel.
	static constexpr u32 kMinKernelVertices = 6;

	// The ADC (skip) bit rides in the side entry meta's spare bits. Bits 0-27
	// are the X band, 28-55 the Y band, 56-59 the outcode; 60-63 are unused by
	// the mirror entry.
	static constexpr u64 kCullMetaAdcBit = 1ull << 60;

	static_assert((kCullMetaAdcBit & (GSVertexKernels::kCullMetaBandXMask |
										 GSVertexKernels::kCullMetaBandYMask |
										 GSVertexKernels::kCullMetaOutcodeMask)) == 0,
		"the ADC bit must not collide with the mirror entry's band or outcode fields");

	// GIFPacked XYZF2 carries ADC in bit 15 of the fourth word (Skip()), so the
	// bit lands in place with one shift.
	static constexpr u32 kAdcShift = 45;
	static_assert((static_cast<u64>(0x8000u) << kAdcShift) == kCullMetaAdcBit);

	// Batch invariants. Everything here is fixed for the whole of a kernel entry.
	struct Invariants
	{
		GSVector4i xyof;            // {ofx, ofy, ofx, ofy}
		GSVertexKernels::CullBounds bounds; // banded bounds at native res
		u32 uv;                     // latched UV (packed XYZF2 does not write UV)
		GSVector4i clamp_keep;      // depth clamp, as two lane masks over m[1]:
		GSVector4i clamp_shifted;   //   m1' = (m1 & keep) | ((m1 >> 8) & shifted)
		// TME, FST and IIP in one word rather than three: they are read only on the
		// accept path.
		u32 shade;                  // bit 0 TME, bit 1 FST, bit 2 IIP
		bool clamp_enabled;
		// Where the batch's last parsed vertex goes -- GSState::m_v, which the
		// piecemeal handlers and the next tag read.
		GSVertex* last_out;
		// The cull grid the accepted-prim bbox rounds onto (always the native
		// grid here; the driver declines the fused path otherwise).
		GSVertexKernels::CullGrid grid;
	};

	// Deferred draw-rect accumulation state, returned beside the rect union.
	enum AccState : u32
	{
		kAccEmpty = 0,
		kAccUnion = 1,
		kAccReplace = 2,
	};

	// {wx, wy, wx, wy} from a side entry's packed position -- the shape
	// ComputeCullBBox's runion consumes, and the shape the xy ring holds.
	__forceinline_odr GSVector4i BroadcastXY(u64 xyp)
	{
#ifdef ARCH_ARM64
		return GSVector4i(vreinterpretq_s32_u64(vdupq_n_u64(xyp)));
#else
		return GSVector4i(_mm_set1_epi64x(static_cast<s64>(xyp)));
#endif
	}

	// The depth-clamp hack as two lane masks, so the clamp costs no branch and no
	// address-of on the parsed vector. m[1] is {XY, Z, UV, FOG}; only the Z lane
	// moves.
	//   Disabled        z' = z
	//   PrioritizeLower z' = z & 0x00FFFFFF
	//   PrioritizeUpper z' = ((z >> 8) & ~0xFF) | (z & 0xFF)
	__forceinline_odr void MakeDepthClampMasks(GSLimit24BitDepth mode, GSVector4i& keep, GSVector4i& shifted)
	{
		switch (mode)
		{
			case GSLimit24BitDepth::PrioritizeLower:
				keep = GSVector4i(-1, 0x00FFFFFF, -1, -1);
				shifted = GSVector4i::zero();
				break;
			case GSLimit24BitDepth::PrioritizeUpper:
				keep = GSVector4i(-1, 0x000000FF, -1, -1);
				shifted = GSVector4i(0, static_cast<int>(0xFFFFFF00u), 0, 0);
				break;
			default:
				keep = GSVector4i::xffffffff();
				shifted = GSVector4i::zero();
				break;
		}
	}

#ifdef ARCH_ARM64
	// The mirror-entry build, four vertices at a time. Verbatim ARMSX2 (see
	// GSVertexKickKernel.h there): byte-exact with MakeCullMirrorEntry<true>.
	//
	// v0..v3 are the raw GIFPacked XYZF2 words of four consecutive vertices:
	// lane 0 carries X in its low half, lane 1 carries Y, lane 3 carries ADC in
	// bit 15.
	struct MirrorBounds
	{
		int32x4_t ofx, ofy, l, t, r, b;
		int32x4_t band_shift;      // negative, so SSHL right-shifts by the grid's log2 step
		int32x4_t bias_x, bias_y;  // the grid's phase plus one (see MakeCullMirrorEntry)
		uint32x4_t banded;         // all-ones when the outcode compares bands, zero for raw 12.4
	};

	__forceinline_odr MirrorBounds MakeMirrorBounds(const GSVector4i& xyof,
		const GSVertexKernels::CullBounds& bounds, int band_shift, bool banded, int bias_x, int bias_y)
	{
		MirrorBounds m;
		m.ofx = vdupq_n_s32(xyof.I32[0]);
		m.ofy = vdupq_n_s32(xyof.I32[1]);
		m.l = vdupq_n_s32(bounds.l);
		m.t = vdupq_n_s32(bounds.t);
		m.r = vdupq_n_s32(bounds.r);
		m.b = vdupq_n_s32(bounds.b);
		m.band_shift = vdupq_n_s32(-band_shift);
		m.bias_x = vdupq_n_s32(bias_x);
		m.bias_y = vdupq_n_s32(bias_y);
		m.banded = vdupq_n_u32(banded ? 0xFFFFFFFFu : 0u);
		return m;
	}

	__forceinline_odr void BuildMirrorQuad(uint32x4_t v0, uint32x4_t v1, uint32x4_t v2, uint32x4_t v3,
		const MirrorBounds& k, u64* RESTRICT xyp_out, u64* RESTRICT meta_out)
	{
		// Transpose to planar X / Y / flags.
		const uint64x2_t a01 = vreinterpretq_u64_u32(vzip1q_u32(v0, v1));
		const uint64x2_t a23 = vreinterpretq_u64_u32(vzip1q_u32(v2, v3));
		const uint64x2_t b01 = vreinterpretq_u64_u32(vzip2q_u32(v0, v1));
		const uint64x2_t b23 = vreinterpretq_u64_u32(vzip2q_u32(v2, v3));
		const uint32x4_t X = vreinterpretq_u32_u64(vzip1q_u64(a01, a23));
		const uint32x4_t Y = vreinterpretq_u32_u64(vzip2q_u64(a01, a23));
		const uint32x4_t F = vreinterpretq_u32_u64(vzip2q_u64(b01, b23));

		// Window position: the raw 12.4 coordinate is the low half-word, and the
		// offset subtract is over the full 32-bit lane, exactly as the ring's.
		const uint32x4_t m16 = vdupq_n_u32(0xFFFFu);
		const int32x4_t wx = vsubq_s32(vreinterpretq_s32_u32(vandq_u32(X, m16)), k.ofx);
		const int32x4_t wy = vsubq_s32(vreinterpretq_s32_u32(vandq_u32(Y, m16)), k.ofy);

		// SSHL by a negative amount is an arithmetic right shift, which is what a
		// runtime band width needs -- the immediate form cannot take one.
		const int32x4_t bx = vshlq_s32(vsubq_s32(wx, k.bias_x), k.band_shift);
		const int32x4_t by = vshlq_s32(vsubq_s32(wy, k.bias_y), k.band_shift);

		// The outcode compares bands at native and raw 12.4 everywhere else; the
		// bands are packed either way.
		const int32x4_t cx = vbslq_s32(k.banded, bx, wx);
		const int32x4_t cy = vbslq_s32(k.banded, by, wy);

		uint32x4_t oc = vandq_u32(vcltq_s32(cx, k.l), vdupq_n_u32(1));
		oc = vorrq_u32(oc, vandq_u32(vcgeq_s32(cx, k.r), vdupq_n_u32(2)));
		oc = vorrq_u32(oc, vandq_u32(vcltq_s32(cy, k.t), vdupq_n_u32(4)));
		oc = vorrq_u32(oc, vandq_u32(vcgeq_s32(cy, k.b), vdupq_n_u32(8)));
		// ADC rides in meta bit 60, four above the outcode field, so it joins the
		// outcode here and lands with it in one shift.
		oc = vorrq_u32(oc, vshrq_n_u32(vandq_u32(F, vdupq_n_u32(0x8000u)), 11));

		// meta, as its two words: low = bandx[0..27] | bandy[0..3] << 28,
		// high = bandy[4..27] | outcode << 24.
		const uint32x4_t lo = vorrq_u32(vandq_u32(vreinterpretq_u32_s32(bx), vdupq_n_u32(0x0FFFFFFFu)),
			vshlq_n_u32(vreinterpretq_u32_s32(by), 28));
		const uint32x4_t hi = vorrq_u32(vandq_u32(vshrq_n_u32(vreinterpretq_u32_s32(by), 4), vdupq_n_u32(0x00FFFFFFu)),
			vshlq_n_u32(oc, 24));

		// One zip per pair and straight out: the two fields live in separate arrays,
		// so nothing has to be interleaved against the other.
		vst1q_u64(xyp_out + 0, vreinterpretq_u64_u32(vzip1q_u32(vreinterpretq_u32_s32(wx), vreinterpretq_u32_s32(wy))));
		vst1q_u64(xyp_out + 2, vreinterpretq_u64_u32(vzip2q_u32(vreinterpretq_u32_s32(wx), vreinterpretq_u32_s32(wy))));
		vst1q_u64(meta_out + 0, vreinterpretq_u64_u32(vzip1q_u32(lo, hi)));
		vst1q_u64(meta_out + 2, vreinterpretq_u64_u32(vzip2q_u32(lo, hi)));
	}
#endif // ARCH_ARM64

	// ------------------------------------------------------------------------
	// Pass one: parse, store, side entry. `clamp` is a template parameter rather
	// than a per-vertex test because the disabled case is the default and must
	// carry no cost at all. The contiguous {STQ, RGBAQ, XYZF2} triple only:
	// stride 3, XYZ at +2, UV latched.
	// ------------------------------------------------------------------------
	template <bool clamp>
	__forceinline_odr void PassOne(const GIFPackedReg* RESTRICT r, u32 count,
		GSVertex* RESTRICT out, u64* RESTRICT side_xyp, u64* RESTRICT side_meta, const Invariants& inv)
	{
		const u32 uv = inv.uv;
		const int ofx = inv.xyof.I32[0];
		const int ofy = inv.xyof.I32[1];
		const int bl = inv.bounds.l, bt = inv.bounds.t, br = inv.bounds.r, bb = inv.bounds.b;
		const int band_shift = inv.grid.shift;
		const bool banded = (band_shift == 4);
		const int band_bias_x = inv.grid.band_bias_x;
		const int band_bias_y = inv.grid.band_bias_y;
		const GSVector4i keep = inv.clamp_keep;
		const GSVector4i shifted = inv.clamp_shifted;
#ifdef ARCH_ARM64
		// Hoisted out of the loop on purpose: left inside the parse they are
		// function-local statics that clang rematerializes from the frame every
		// iteration.
		const GSVertexKernels::PackedParseConsts kc = GSVertexKernels::MakePackedParseConsts();
		const MirrorBounds mb = MakeMirrorBounds(inv.xyof, inv.bounds, band_shift, banded, band_bias_x, band_bias_y);
#endif

		// The vertex parse is per vertex (one TBL pair each); the mirror build is
		// four at a time. The remainder does not get a scalar mirror build: when
		// there are four vertices to look back on, one more quad starting at
		// count - 4 covers it. Only a run shorter than four vertices takes the
		// scalar path below, which is also the whole of the non-aarch64 path.
		u32 i = 0;
#ifdef ARCH_ARM64
		for (const u32 quads = count & ~3u; i < quads; i += 4)
		{
			for (u32 j = 0; j < 4; j++)
			{
				const GIFPackedReg* RESTRICT rv = r + (i + j) * 3;
				GSVector4i m0, m1;
				GSVertexKernels::ParsePackedSTQRGBAXYZF2_Fast(rv, uv, kc, m0, m1);

				if constexpr (clamp)
					m1 = (m1 & keep) | (m1.srl32<8>() & shifted);

				out[i + j].m[0] = m0;
				out[i + j].m[1] = m1;
			}

			BuildMirrorQuad(
				vld1q_u32(reinterpret_cast<const u32*>(r + (i + 0) * 3 + 2)),
				vld1q_u32(reinterpret_cast<const u32*>(r + (i + 1) * 3 + 2)),
				vld1q_u32(reinterpret_cast<const u32*>(r + (i + 2) * 3 + 2)),
				vld1q_u32(reinterpret_cast<const u32*>(r + (i + 3) * 3 + 2)),
				mb, side_xyp + i, side_meta + i);
		}

		if (i < count && count >= 4)
		{
			const u32 back = count - 4;
			BuildMirrorQuad(
				vld1q_u32(reinterpret_cast<const u32*>(r + (back + 0) * 3 + 2)),
				vld1q_u32(reinterpret_cast<const u32*>(r + (back + 1) * 3 + 2)),
				vld1q_u32(reinterpret_cast<const u32*>(r + (back + 2) * 3 + 2)),
				vld1q_u32(reinterpret_cast<const u32*>(r + (back + 3) * 3 + 2)),
				mb, side_xyp + back, side_meta + back);

			for (; i < count; i++)
			{
				const GIFPackedReg* RESTRICT rv = r + i * 3;
				GSVector4i m0, m1;
				GSVertexKernels::ParsePackedSTQRGBAXYZF2_Fast(rv, uv, kc, m0, m1);

				if constexpr (clamp)
					m1 = (m1 & keep) | (m1.srl32<8>() & shifted);

				out[i].m[0] = m0;
				out[i].m[1] = m1;
			}
			return;
		}
#endif

		for (; i < count; i++)
		{
			const GIFPackedReg* RESTRICT rv = r + i * 3;

			GSVector4i m0, m1;
#ifdef ARCH_ARM64
			GSVertexKernels::ParsePackedSTQRGBAXYZF2_Fast(rv, uv, kc, m0, m1);
#else
			GSVertexKernels::ParsePackedSTQRGBAXYZF2_Fast(rv, uv, m0, m1);
#endif

			if constexpr (clamp)
				m1 = (m1 & keep) | (m1.srl32<8>() & shifted);

			out[i].m[0] = m0;
			out[i].m[1] = m1;

			// The window position and its cull metadata. Same expressions as
			// MakeCullMirrorEntry, with the ADC bit folded into the spare meta
			// bits -- and the same expressions BuildMirrorQuad evaluates lane-wise.
			const u32 raw = rv[2].U32[0];
			const u32 raw_y = rv[2].U32[1];
			const int wx = static_cast<int>(raw & 0xFFFFu) - ofx;
			const int wy = static_cast<int>(raw_y & 0xFFFFu) - ofy;
			const int bx = (wx - band_bias_x) >> band_shift;
			const int by = (wy - band_bias_y) >> band_shift;
			const int cx = banded ? bx : wx;
			const int cy = banded ? by : wy;

			u32 oc = 0;
			oc |= (cx < bl) ? 1u : 0u;
			oc |= (cx >= br) ? 2u : 0u;
			oc |= (cy < bt) ? 4u : 0u;
			oc |= (cy >= bb) ? 8u : 0u;

			side_xyp[i] = static_cast<u64>(static_cast<u32>(wx)) | (static_cast<u64>(static_cast<u32>(wy)) << 32);
			side_meta[i] = (static_cast<u64>(static_cast<u32>(bx)) & GSVertexKernels::kCullMetaBandXMask) |
			               ((static_cast<u64>(static_cast<u32>(by)) << 28) & GSVertexKernels::kCullMetaBandYMask) |
			               (static_cast<u64>(oc) << 56) |
			               (static_cast<u64>(rv[2].U32[3] & 0x8000u) << kAdcShift);
		}
	}

	// ------------------------------------------------------------------------
	// The kernel, triangle strips. The caller guarantees:
	//   * itail != 0, so the per-draw environment snapshot cannot fire inside;
	//   * draw buffering is off (no overlap check, no buffer switch);
	//   * the scissor is valid, so the ADC bit is the whole pre-cull rejection;
	//   * native res with no AA1 expansion, so the scalar-outcode cull decides;
	//   * tail + count + 3 <= maxcount, so no growth can be needed;
	//   * tail + count < MaxVerticesForPrim, so no VERTEXCOUNT flush can be
	//     needed.
	// Every vertex of the chunk is consumed; the caller advances by `count`.
	// VBuf/IBuf are GSState::GSVertexBuff/GSIndexBuff (template parameters so
	// this header does not need GSState).
	// ------------------------------------------------------------------------
	template <typename VBuf, typename IBuf>
	__noinline GSVector4i RunChunk(const GIFPackedReg* RESTRICT rin, u32 count,
		VBuf* RESTRICT vertex_buf, IBuf* RESTRICT index_buf,
		u64* RESTRICT side_xyp, u64* RESTRICT side_meta, const Invariants& inv, u32* RESTRICT acc_state_out)
	{
		static constexpr u32 n = 3;
		static constexpr int primclass = GS_TRIANGLE_CLASS;

		GSVertex* RESTRICT vbuff = vertex_buf->buff;
		u16* RESTRICT ibuff = index_buf->buff;

		const u32 tail0 = vertex_buf->tail;
		const u32 xy_tail0 = vertex_buf->xy_tail;

		if (inv.clamp_enabled)
			PassOne<true>(rin, count, vbuff + tail0, side_xyp, side_meta, inv);
		else
			PassOne<false>(rin, count, vbuff + tail0, side_xyp, side_meta, inv);

		// m_v carries the last parsed vertex out of the batch. Pass one has just
		// written it to its provisional slot and pass two has not run yet, so
		// nothing has moved it.
		*inv.last_out = vbuff[tail0 + count - 1];

		// ---- pass two -------------------------------------------------------
		u32 head = vertex_buf->head;
		u32 tail = tail0;
		u32 next = vertex_buf->next;
		u32 itail = index_buf->tail;
		// The index write cursor walks: itail only ever grows inside a chunk.
		u16* RESTRICT ib = ibuff + itail;
		u32 watermark = vertex_buf->fmm_watermark;
		bool fmm_valid = vertex_buf->fmm_valid;
		u32 acc_state = kAccEmpty;
		GSVector4i acc_rect = GSVector4i::zero();

		const u32 shade = inv.shade;
		const bool tme = (shade & 1u) != 0;
		const bool fst = (shade & 2u) != 0;
		const bool iip = (shade & 4u) != 0;

		GSVertexKernels::FmmAcc acc;
		bool fmm_dirty = false;
#ifdef ARCH_ARM64
		if (fmm_valid)
		{
			// Only meaningful while fmm_valid; the reset at the first emission of
			// a draw initializes it for real.
			acc = vertex_buf->fmm_acc;
		}
		else
#endif
		{
			GSVertexKernels::FmmAccReset(acc, false, false);
		}

		// The three most recent mirror entries, most recent first. Rebuilt from
		// the xy ring (GP3: no kick_ring) because a chunk can begin mid-prim;
		// after that they rotate in registers and the ring is not read again.
		// Rebuilding from positions with the current bounds is immune to a
		// mid-draw PRIM-class change, which can strand stale-class outcodes in a
		// maintained ring.
		const GSVector4i seed0 = vertex_buf->xy[(xy_tail0 - 1) & 3];
		const GSVector4i seed1 = vertex_buf->xy[(xy_tail0 - 2) & 3];
		const GSVector4i seed2 = vertex_buf->xy[(xy_tail0 - 3) & 3];
		GSVertexKernels::CullMirrorEntry s0 =
			GSVertexKernels::MakeCullMirrorEntry<true>(seed0.I32[0], seed0.I32[1], inv.bounds, 4);
		GSVertexKernels::CullMirrorEntry s1 =
			GSVertexKernels::MakeCullMirrorEntry<true>(seed1.I32[0], seed1.I32[1], inv.bounds, 4);
		GSVertexKernels::CullMirrorEntry s2 =
			GSVertexKernels::MakeCullMirrorEntry<true>(seed2.I32[0], seed2.I32[1], inv.bounds, 4);
		u64 xyp0 = s0.xyp, meta0 = s0.meta;
		u64 xyp1 = s1.xyp, meta1 = s1.meta;
		u64 xyp2 = s2.xyp, meta2 = s2.meta;

		// Walked rather than indexed: the provisional cursor advances by exactly
		// one vertex an iteration, so it is a post-incremented pointer instead of
		// an address recomputed from tail0 + i every time.
		const GSVertex* RESTRICT prov = vbuff + tail0;

		for (u32 i = 0; i < count; i++)
		{
			xyp2 = xyp1;
			meta2 = meta1;
			xyp1 = xyp0;
			meta1 = meta0;
			xyp0 = side_xyp[i];
			meta0 = side_meta[i];

			// Move the vertex from its provisional slot to the live tail, so the
			// buffer below tail is what the per-vertex kick would have left there
			// -- including the slots a rejected strip vertex passes through, which
			// nothing indexes, but which the per-vertex kick would have written. The live tail
			// never runs ahead of the provisional cursor, so the destination is at
			// or below the source and a later vertex's source is never written
			// over; when they coincide (an unbroken run of accepts, or any chunk
			// with no compaction in it) this is a self-copy the store buffer eats.
			vbuff[tail] = *prov++;

			tail++;
			if ((tail - head) < n)
				continue;

			// The ADC bit is the whole rejection test before the cull: a run with an
			// invalid scissor never reaches the kernel (the driver keeps it on the
			// per-vertex path), so nothing else has to be OR'd in here.
			u32 skip = static_cast<u32>((meta0 >> 60) & 1u);
			if (skip == 0)
			{
				const GSVertexKernels::CullMirrorEntry e0{xyp0, meta0};
				const GSVertexKernels::CullMirrorEntry e1{xyp1, meta1};
				const GSVertexKernels::CullMirrorEntry e2{xyp2, meta2};
				skip = GSVertexKernels::CullTestScalar<n, primclass>(e0, e1, e2);
			}

			if (skip != 0)
			{
				// A rejected strip vertex stays in the buffer until the next
				// accept compacts over it.
				head = head + 1;
				continue;
			}

			// The strip compaction, exactly as the per-vertex kick does it: the
			// window slides down over the vertices the rejections left behind.
			u32 dst = head;
			if (next < head)
			{
				vbuff[next + 0] = vbuff[head + 0];
				vbuff[next + 1] = vbuff[head + 1];
				vbuff[next + 2] = vbuff[head + 2];
				dst = next;
#ifdef ARCH_ARM64
				// Vertices moved below the fused-FMM watermark must re-accumulate.
				watermark = std::min(watermark, next);
#endif
			}

			const GSVector4i bbox = GSVertexKernels::ComputeCullBBox<n, primclass>(
				BroadcastXY(xyp0), BroadcastXY(xyp1), BroadcastXY(xyp2), inv.grid, false);

			ib[0] = static_cast<u16>(dst + 0);
			ib[1] = static_cast<u16>(dst + 1);
			ib[2] = static_cast<u16>(dst + 2);
			head = dst + 1;
			next = dst + 3;
			tail = dst + 3;
			itail += 3;
			ib += 3;

#ifdef ARCH_ARM64
			{
				const u32 last = tail - 1;
				if (itail == n)
				{
					GSVertexKernels::FmmAccReset(acc, tme, fst);
					fmm_valid = true;
					watermark = last - 2;
				}

				if (fmm_valid)
				{
					for (u32 j = std::max(watermark, last - 2); j < last; j++)
					{
						GSVertexKernels::FmmAccumVertex(acc, GSVector4i(vbuff[j].m[0]),
							GSVector4i(vbuff[j].m[1]), tme, fst, iip);
					}
					GSVertexKernels::FmmAccumVertex(acc, GSVector4i(vbuff[last].m[0]),
						GSVector4i(vbuff[last].m[1]), tme, fst, true);
					watermark = last + 1;
					fmm_dirty = true;
				}
			}
#endif

			const GSVector4i draw_rect = GSVertexKernels::PrimDrawRect(bbox);
			if (acc_state != 0)
				acc_rect = acc_rect.runion(draw_rect);
			else
			{
				acc_rect = draw_rect;
				acc_state = (itail == n) ? kAccReplace : kAccUnion;
			}
		}

		// ---- exit -----------------------------------------------------------
		// The xy ring holds the last four kicked vertices. Nothing reads it
		// mid-chunk: every reader runs inside a flush, and a flush only happens
		// inside a legacy kick.
		{
			GSVector4i* RESTRICT xy_ring = vertex_buf->xy;
			const u32 xyt = xy_tail0;

#ifdef ARCH_ARM64
			if (count >= 4)
			{
				// The four entries are four consecutive u64s of the position side
				// array, so the whole writeback is two vector loads and four zips:
				// the xy ring's entry is the position broadcast to both halves,
				// which is a zip of the position with itself.
				const u32 j0 = count - 4;
				const uint64x2_t p01 = vld1q_u64(side_xyp + j0);
				const uint64x2_t p23 = vld1q_u64(side_xyp + j0 + 2);

				u64* RESTRICT xr = reinterpret_cast<u64*>(xy_ring);
				const u32 s0 = (xyt + j0) & 3;
				const u32 s1 = (s0 + 1) & 3, s2 = (s0 + 2) & 3, s3 = (s0 + 3) & 3;

				vst1q_u64(xr + s0 * 2, vzip1q_u64(p01, p01));
				vst1q_u64(xr + s1 * 2, vzip2q_u64(p01, p01));
				vst1q_u64(xr + s2 * 2, vzip1q_u64(p23, p23));
				vst1q_u64(xr + s3 * 2, vzip2q_u64(p23, p23));
			}
			else
#else
			if (count >= 4)
			{
				const u32 j0 = count - 4;
				xy_ring[(xyt + j0 + 0) & 3] = BroadcastXY(side_xyp[j0 + 0]);
				xy_ring[(xyt + j0 + 1) & 3] = BroadcastXY(side_xyp[j0 + 1]);
				xy_ring[(xyt + j0 + 2) & 3] = BroadcastXY(side_xyp[j0 + 2]);
				xy_ring[(xyt + j0 + 3) & 3] = BroadcastXY(side_xyp[j0 + 3]);
			}
			else
#endif
			{
				for (u32 j = 0; j < count; j++)
					xy_ring[(xyt + j) & 3] = BroadcastXY(side_xyp[j]);
			}
		}

#ifdef ARCH_ARM64
		if (fmm_dirty)
			vertex_buf->fmm_acc = acc;
#else
		(void)fmm_dirty;
#endif

		vertex_buf->head = head;
		vertex_buf->tail = tail;
		vertex_buf->next = next;
		vertex_buf->xy_tail = xy_tail0 + count;
		vertex_buf->fmm_watermark = watermark;
		vertex_buf->fmm_valid = fmm_valid;
		index_buf->tail = itail;

		*acc_state_out = acc_state;
		return acc_rect;
	}
} // namespace GSVertexKickKernel
