// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the terrain pass-fusion matcher (GS/Renderers/Common/GSTerrainFusePolicy.h):
// SSX3 draws each terrain patch as three consecutive draws over byte-identical
// XYZ -- T1 base (replace), T3 shadow ((Cd-Cs)*As), T2 light (Cs*Ad+Cd).
//
// The positive vectors are exact register words from the Elysium capture
// (tpf1_census.py over RTC1 elys-cap1.c2, tick 2997; the same triple shape
// covers 6592/6675 xyz-identical triples there and 29620 ap120 stock-race
// triples). The refusal half flips every templated field: a draw outside the
// template must pass through unfused, since the fused shader only reproduces
// the templated sampling/combine. The compatibility half pins the equality
// rule: everything outside the per-pass template must match across the triple
// (any value), except PRIM which may differ only in the CTXT bit.
//
// Rides gs_vertex_tests -- the policy is header-only, so it needs no extra linkage.

#include "GS/Renderers/Common/GSTerrainFusePolicy.h"

#include <gtest/gtest.h>

namespace
{
using GSTerrainFusePolicy::DrawRegs;
using GSTerrainFusePolicy::Pass;

// Censused Elysium tick-2997 triple (raw register words).
DrawRegs CensusedT1()
{
	DrawRegs r = {};
	r.prim = 0x5c;
	r.tex0 = 0x20063925dd30bd3dULL; // T8 CLUT 128x128, TCC 1, TFX 0
	r.tex1 = 0x16c; // LCM 0, MXL 3, MMAG 1, MMIN 5
	r.clamp = 0x0; // repeat/repeat
	r.alpha = 0x2a; // (2,2,2,0)
	r.test = 0x51143; // ATE, ATST ALWAYS, AFAIL FB_ONLY, ZTE, ZTST GEQUAL
	r.frame = 0x80000; // FBP/FBW/PSM, FBMSK 0
	r.zbuf = 0x10000e0;
	r.texa = 0x80000080;
	r.texclut = 0x0;
	r.xyoffset = 0x720000007000ULL;
	r.scissor = 0x1bf000001ff0000ULL;
	r.miptbp2 = 0x0;
	r.tex2 = 0x0;
	r.fogcol = 0x0;
	r.fba = 0;
	r.pabe = 0;
	r.colclamp = 1;
	r.dthe = 0;
	r.scanmsk = 0;
	r.dimx = 0x2637405137265140ULL;
	return r;
}

DrawRegs CensusedT3()
{
	DrawRegs r = CensusedT1();
	r.prim = 0x25c; // same + CTXT
	r.tex0 = 0x5dc00b6c1ULL; // CT32 128x128, TCC 1, TFX 0
	r.tex1 = 0x61; // LCM 1, MXL 0, MMAG 1, MMIN 1
	r.clamp = 0x5; // clamp/clamp
	r.alpha = 0x81; // (1,0,0,2)
	r.frame = 0xff00000000080000ULL; // FBMSK ff000000
	return r;
}

DrawRegs CensusedT2()
{
	DrawRegs r = CensusedT1();
	r.prim = 0x25c;
	r.tex0 = 0x554006ac4ULL; // CT32 32x32, TCC 1, TFX 0
	r.tex1 = 0x164; // LCM 0, MXL 1, MMAG 1, MMIN 5
	r.clamp = 0x5;
	r.alpha = 0x58; // (0,2,1,1)
	r.frame = 0xff00000000080000ULL;
	return r;
}

TEST(TerrainFusePolicy, ClassifiesCensusedTriple)
{
	EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(CensusedT1()), Pass::Base);
	EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(CensusedT3()), Pass::Shadow);
	EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(CensusedT2()), Pass::Light);
}

TEST(TerrainFusePolicy, RefusesNonTemplatePasses)
{
	// Wrong blend equations.
	for (u64 a : {0x00ULL, 0x2bULL, 0x80ULL, 0x59ULL})
	{
		DrawRegs r = CensusedT1();
		r.alpha = a;
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None) << std::hex << a;
	}
	// T1 with a T3/T2 blend is not a base (and not a shadow/light: FBMSK/PSM mismatch).
	for (u64 a : {0x81ULL, 0x58ULL})
	{
		DrawRegs r = CensusedT1();
		r.alpha = a;
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None) << std::hex << a;
	}
	// 16-bit framebuffer refuses (shuffle verdict not provable).
	{
		DrawRegs r = CensusedT1();
		r.frame |= (2ULL << 24); // CT16
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None);
	}
	// FBMSK variants.
	{
		DrawRegs r = CensusedT1();
		r.frame |= 0xff00000000000000ULL;
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None);
	}
	{
		DrawRegs r = CensusedT3();
		r.frame &= 0x00000000ffffffffULL;
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None);
	}
	// PSM variants: non-paletted base, paletted shadow/light.
	{
		DrawRegs r = CensusedT1();
		r.tex0 = (r.tex0 & ~(0x3fULL << 20)) | (0ULL << 20); // CT32
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None);
	}
	{
		DrawRegs r = CensusedT3();
		r.tex0 = (r.tex0 & ~(0x3fULL << 20)) | (19ULL << 20); // T8
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None);
	}
	// T4 base is accepted (ap120 stock race has T4-base triples).
	{
		DrawRegs r = CensusedT1();
		r.tex0 = (r.tex0 & ~(0x3fULL << 20)) | (20ULL << 20); // T4
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::Base);
	}
	// TCC/TFX variants.
	{
		DrawRegs r = CensusedT1();
		r.tex0 ^= (1ULL << 34); // TCC 0
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None);
	}
	{
		DrawRegs r = CensusedT2();
		r.tex0 |= (1ULL << 35); // TFX decal
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None);
	}
	// CLAMP variants.
	{
		DrawRegs r = CensusedT1();
		r.clamp = 0x5;
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None);
	}
	{
		DrawRegs r = CensusedT3();
		r.clamp = 0x0;
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None);
	}
	// Nearest mag filter (MMAG 0).
	{
		DrawRegs r = CensusedT1();
		r.tex1 &= ~(1u << 5);
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None);
	}
	// Mipmap-shape fields are free (per-pass LOD is replicated, self-checked on T1).
	{
		DrawRegs r = CensusedT3();
		r.tex1 = 0x1a4; // MXL 1, MMIN 5 like T2
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::Shadow);
	}
	// PRIM variants: sprite class, ABE off, FST/FGE/AA1/FIX on, TME off.
	for (u32 p : {0x50u, 0x1cu, 0x15cu, 0x7cu, 0xdcu, 0x45cu, 0x4cu})
	{
		DrawRegs r = CensusedT1();
		r.prim = p;
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None) << std::hex << p;
	}
	// CTXT bit is free (passes read their own context).
	{
		DrawRegs r = CensusedT1();
		r.prim ^= (1u << 9);
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::Base);
	}
	// TEST variants: ATE off, real alpha test, DATE on, ZTE off, ZTST ALWAYS.
	for (u64 t : {0x51142ULL, 0x51145ULL, 0x55143ULL, 0x41143ULL, 0x31143ULL})
	{
		DrawRegs r = CensusedT2();
		r.test = t;
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None) << std::hex << t;
	}
	// AREF/AFAIL/DATM values are free (dead under ATST ALWAYS / DATE off).
	{
		DrawRegs r = CensusedT2();
		r.test ^= (0xabULL << 4) | (2ULL << 12) | (1ULL << 15);
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::Light);
	}
	// Output-transform knobs.
	{
		DrawRegs r = CensusedT1();
		r.fba = 1;
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None);
		r = CensusedT1();
		r.pabe = 1;
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None);
		r = CensusedT1();
		r.colclamp = 0;
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None);
		r = CensusedT1();
		r.dthe = 1;
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None);
		r = CensusedT1();
		r.scanmsk = 1;
		EXPECT_EQ(GSTerrainFusePolicy::ClassifyPass(r), Pass::None);
	}
}

TEST(TerrainFusePolicy, CompatibilityNeedsEqualityOutsideTemplate)
{
	const DrawRegs t1 = CensusedT1(), t3 = CensusedT3(), t2 = CensusedT2();
	EXPECT_TRUE(GSTerrainFusePolicy::StatesCompatible(t1, t3, t2));

	// Any equality-field divergence refuses ...
	DrawRegs bad = t2;
	bad.test ^= 1ULL << 20; // reserved bit; breaks cross-pass equality
	EXPECT_FALSE(GSTerrainFusePolicy::StatesCompatible(t1, t3, bad));
	bad = t2;
	bad.zbuf ^= 1ULL << 32; // ZMSK flip
	EXPECT_FALSE(GSTerrainFusePolicy::StatesCompatible(t1, t3, bad));
	bad = t2;
	bad.frame ^= 1ULL << 4; // FBP flip
	EXPECT_FALSE(GSTerrainFusePolicy::StatesCompatible(t1, t3, bad));
	bad = t3;
	bad.prim ^= 1u << 3; // IIP flip (CTXT alone is free)
	EXPECT_FALSE(GSTerrainFusePolicy::StatesCompatible(t1, bad, t2));
	bad = t3;
	bad.texa ^= 0x80ULL;
	EXPECT_FALSE(GSTerrainFusePolicy::StatesCompatible(t1, bad, t2));
	bad = t3;
	bad.scissor ^= 1ULL;
	EXPECT_FALSE(GSTerrainFusePolicy::StatesCompatible(t1, bad, t2));
	bad = t3;
	bad.xyoffset ^= 1ULL;
	EXPECT_FALSE(GSTerrainFusePolicy::StatesCompatible(t1, bad, t2));

	// ... but the per-pass template fields and CTXT may vary.
	DrawRegs t1b = t1, t3b = t3, t2b = t2;
	t1b.prim ^= (1u << 9);
	t3b.prim ^= (1u << 9);
	EXPECT_TRUE(GSTerrainFusePolicy::StatesCompatible(t1b, t3b, t2b));
}

TEST(TerrainFusePolicy, VertexStreamsMatchIgnoresSTOnly)
{
	alignas(32) u8 v0[64] = {}, v1[64] = {};
	u16 i0[4] = {0, 1, 2, 3}, i1[4] = {0, 1, 2, 3};
	// Fill RGBAQ+XYZ identically, ST differently.
	for (int v = 0; v < 2; v++)
	{
		for (int k = 8; k < 24; k++)
		{
			v0[v * 32 + k] = v1[v * 32 + k] = static_cast<u8>(v * 16 + k);
		}
		for (int k = 0; k < 8; k++)
		{
			v0[v * 32 + k] = static_cast<u8>(k);
			v1[v * 32 + k] = static_cast<u8>(k + 40);
		}
	}
	EXPECT_TRUE(GSTerrainFusePolicy::VertexStreamsMatch(v0, v1, 2, i0, i1, 4));
	// XYZ flip refuses.
	v1[16] ^= 0xff;
	EXPECT_FALSE(GSTerrainFusePolicy::VertexStreamsMatch(v0, v1, 2, i0, i1, 4));
	v1[16] ^= 0xff;
	// RGBAQ flip refuses.
	v1[8 + 3] ^= 0xff;
	EXPECT_FALSE(GSTerrainFusePolicy::VertexStreamsMatch(v0, v1, 2, i0, i1, 4));
	v1[8 + 3] ^= 0xff;
	// UV/FOG flips are ignored (dead under FST=0/FGE=0).
	v1[24] ^= 0xff;
	v1[28] ^= 0xff;
	EXPECT_TRUE(GSTerrainFusePolicy::VertexStreamsMatch(v0, v1, 2, i0, i1, 4));
	// Index flip refuses; empty draw refuses.
	i1[2] ^= 0xff;
	EXPECT_FALSE(GSTerrainFusePolicy::VertexStreamsMatch(v0, v1, 2, i0, i1, 4));
	EXPECT_FALSE(GSTerrainFusePolicy::VertexStreamsMatch(v0, v1, 0, i0, i1, 0));
}

} // namespace
