// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// RZV1 S4b/S4c: one static-world (compact) packet's kick outcome, computed
// without the kick. ApplyPRIM resets the vertex queue at every packet (head =
// tail = next), and the handler's CheckFlushes is idempotent, so once it has
// run the outcome depends only on the packet and on the cull state below; the
// slot base and whether the draw is still empty are applied by
// GSState::StaticApply. Pure function of its inputs (no GSState), so it can run
// on any thread against a published state.
//
// Strip and list only, at most kStaticMaxVerts vertices. GSState::StaticFastOk
// lists the kick paths that are NOT this function (auto flush, AA1 expansion,
// the shift-0 legacy cull, an invalid scissor, the draw-buffering overlap
// check, a VERTEXCOUNT flush); those packets keep the kick.

#include "GS/GSCompactRecord.h"
#include "GS/GSVertexKick.h"
#include "GS/GSVertexKickKernel.h"

#include <cstring>

static constexpr u32 kStaticMaxVerts = 128;

// Everything StaticPrepare reads besides the packet, as plain bytes: equal
// bytes give equal outputs. Zero-initialized as a whole (padding included), so
// memcmp is the equality.
struct alignas(16) GSStaticCullState
{
	GSVertexKernels::CullGrid grid;     // the cull grid (bbox rounding, shift, band bias)
	GSVertexKernels::CullBounds bounds; // the triangle class's mirror bounds: band at shift 4, else raw
	s32 xyof_x, xyof_y;                 // m_xyof
	u32 uv;                             // the latched UV (m_v.UV)
	u32 prim;                           // the effective PRIM register after ApplyPRIM (low word)
	s32 clamp;                          // GetDepthClampMode()
	u8 shift0;                          // GKV1Shift0Kernel() && grid.shift == 0
	u8 kernel_ok;                       // KickKernelApplies<triangle class>()
	u8 scissor_invalid;                 // m_scissor_invalid
	u8 pad0;
	u32 pad1[2];
};
static_assert(sizeof(GSStaticCullState) == 112);

struct GSStaticPrep
{
	u32 count;  // packet vertices
	u32 ntri;   // emitted triangles
	u32 nslot;  // relative next after the packet (when ntri > 0)
	u32 head, tail; // relative head/tail after the packet
	u32 wm_min; // the lowest strip-compaction target (relative), ~0u if none
	u32 nring;  // min(count, 4): the ring entries below, oldest first
	u32 pad;
	u64 ring_xyp[4], ring_meta[4]; // the last nring vertices' mirror entries (meta without ADC)
	GSVertexKernels::FmmAcc acc;   // fused-FMM union over the emitted triangles (FmmAccReset when none)
	GSVector4i rect, nrect;        // draw-rect unions over the emitted triangles (pre-scissor)
	u8 slot[3 * kStaticMaxVerts];  // emitted indices, relative
	u8 src[kStaticMaxVerts];       // which packet vertex each relative slot in [0, tail) holds
};

namespace GSStatic
{
	__forceinline_odr bool PrimTME(u32 prim) { return (prim >> 4) & 1u; }
	__forceinline_odr bool PrimIIP(u32 prim) { return (prim >> 3) & 1u; }
	__forceinline_odr bool PrimFST(u32 prim) { return (prim >> 8) & 1u; }

	// GSState::MakeKickMirror for the triangle class, from the state's bytes.
	__forceinline_odr GSVertexKernels::CullMirrorEntry Mirror(const GSStaticCullState& cs, int wx, int wy)
	{
		const int shift = cs.grid.shift;
		if (shift == 4)
			return GSVertexKernels::MakeCullMirrorEntry<true>(wx, wy, cs.bounds, 4);
		return GSVertexKernels::MakeCullMirrorEntry<false>(wx, wy, cs.bounds, (shift != 0) ? shift : 4,
			cs.grid.band_bias_x, cs.grid.band_bias_y);
	}

	// The vertex as the kick stores it (ParseCompactXYZF2 + the depth clamp).
	__forceinline_odr void Parse(const Ge1CompactVertex* RESTRICT d, u32 uv, bool clamp, const GSVector4i& keep,
		const GSVector4i& shifted, GSVector4i& m0, GSVector4i& m1)
	{
		GSVertexKernels::ParseCompactXYZF2(d, uv, m0, m1);
		if (clamp)
			m1 = (m1 & keep) | (m1.srl32<8>() & shifted);
	}

	template <u32 prim>
	inline void Prepare(const Ge1CompactVertex* RESTRICT d, u32 count, const GSStaticCullState& cs, GSStaticPrep& o)
	{
		static_assert(prim == GS_TRIANGLESTRIP || prim == GS_TRIANGLELIST);
		constexpr bool strip = (prim == GS_TRIANGLESTRIP);
		const bool tme = PrimTME(cs.prim), fst = PrimFST(cs.prim), iip = PrimIIP(cs.prim);
		const bool clamp = static_cast<GSLimit24BitDepth>(cs.clamp) != GSLimit24BitDepth::Disabled;
		GSVector4i keep = GSVector4i::xffffffff(), shifted = GSVector4i::zero();
		if (clamp)
			GSVertexKickKernel::MakeDepthClampMasks(static_cast<GSLimit24BitDepth>(cs.clamp), keep, shifted);
		const GSVertexKernels::CullGrid& grid = cs.grid;
		const bool shift0 = cs.shift0 != 0;

		GSVector4i m0[kStaticMaxVerts], m1[kStaticMaxVerts];
		u64 xyp[kStaticMaxVerts], meta[kStaticMaxVerts];
		for (u32 i = 0; i < count; i++)
		{
			Parse(d + i, cs.uv, clamp, keep, shifted, m0[i], m1[i]);
			const int wx = static_cast<int>(d[i].X & 0xFFFFu) - cs.xyof_x;
			const int wy = static_cast<int>(d[i].Y & 0xFFFFu) - cs.xyof_y;
			const GSVertexKernels::CullMirrorEntry e = Mirror(cs, wx, wy);
			xyp[i] = e.xyp;
			meta[i] = e.meta | (static_cast<u64>(d[i].W3 & 0x8000u) << 45);
		}

		o.count = count;
		GSVertexKernels::FmmAccReset(o.acc, tme, fst);
		o.ntri = 0;
		o.wm_min = ~0u;
		o.rect = GSVector4i::zero();
		o.nrect = GSVector4i::zero();
		u32 head = 0, tail = 0, next = 0;
		for (u32 i = 0; i < count; i++)
		{
			o.src[tail] = static_cast<u8>(i);
			tail++;
			if ((tail - head) < 3)
				continue;
			const GSVertexKernels::CullMirrorEntry e0{xyp[i], meta[i]};
			const GSVertexKernels::CullMirrorEntry e1{xyp[i - 1], meta[i - 1]};
			const GSVertexKernels::CullMirrorEntry e2{xyp[i - 2], meta[i - 2]};
			u32 skip = static_cast<u32>((meta[i] >> 60) & 1u);
			GSVector4i bbox = GSVector4i::zero();
			if (skip == 0)
			{
				if (shift0)
				{
					const u64 all_out = e0.meta & e1.meta & e2.meta;
					if ((all_out & GSVertexKernels::kCullMetaOutcodeMask) != 0)
						skip = 1;
					else if (e0.xyp == e1.xyp || e1.xyp == e2.xyp || e0.xyp == e2.xyp)
						skip = 1;
					if (skip == 0)
					{
						bbox = GSVertexKernels::ComputeCullBBox<3, GS_TRIANGLE_CLASS>(GSVertexKickKernel::BroadcastXY(xyp[i]),
							GSVertexKickKernel::BroadcastXY(xyp[i - 1]), GSVertexKickKernel::BroadcastXY(xyp[i - 2]), grid, false);
						if (bbox.rempty())
							skip = 1;
					}
				}
				else
					skip = GSVertexKernels::CullTestScalar<3, GS_TRIANGLE_CLASS>(e0, e1, e2);
			}
			if (skip != 0)
			{
				if constexpr (strip)
					head = head + 1;
				else
					tail = head;
				continue;
			}
			u32 dst = head;
			if constexpr (strip)
			{
				if (next < head)
				{
					o.src[next + 0] = o.src[head + 0];
					o.src[next + 1] = o.src[head + 1];
					o.src[next + 2] = o.src[head + 2];
					dst = next;
					o.wm_min = std::min(o.wm_min, next);
				}
			}
			if (!shift0)
				bbox = GSVertexKernels::ComputeCullBBox<3, GS_TRIANGLE_CLASS>(GSVertexKickKernel::BroadcastXY(xyp[i]),
					GSVertexKickKernel::BroadcastXY(xyp[i - 1]), GSVertexKickKernel::BroadcastXY(xyp[i - 2]), grid, false);
			u8* s3 = o.slot + 3 * o.ntri;
			s3[0] = static_cast<u8>(dst);
			s3[1] = static_cast<u8>(dst + 1);
			s3[2] = static_cast<u8>(dst + 2);
			for (u32 j = 0; j < 3; j++)
			{
				const u32 v = o.src[dst + j];
				GSVertexKernels::FmmAccumVertex(o.acc, m0[v], m1[v], tme, fst, iip || j == 2);
			}
			const GSVector4i r = GSVertexKernels::PrimDrawRect(bbox);
			const GSVector4i nr = GSVertexKernels::PrimNativeDrawRectOrNone<GS_TRIANGLE_CLASS>(bbox);
			o.rect = o.ntri ? o.rect.runion(r) : r;
			o.nrect = o.ntri ? o.nrect.runion(nr) : nr;
			o.ntri++;
			if constexpr (strip)
			{
				head = dst + 1;
				next = tail = dst + 3;
			}
			else
				head = next = tail = dst + 3;
		}
		o.nslot = next;
		o.head = head;
		o.tail = tail;
		o.nring = std::min<u32>(count, 4);
		for (u32 j = 0; j < o.nring; j++)
		{
			const u32 v = count - o.nring + j;
			o.ring_xyp[j] = xyp[v];
			o.ring_meta[j] = meta[v] & ~GSVertexKickKernel::kCullMetaAdcBit;
		}
	}
} // namespace GSStatic
