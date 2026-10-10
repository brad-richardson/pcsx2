// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// TPF1: terrain pass-fusion matcher. SSX3 draws every terrain patch as three
// consecutive draws over byte-identical XYZ (LEV1 section 1.3, censused exact in
// local/research/TPF1/tooling/tpf1_census.py over Elysium t3000-3010 and the
// ap120 stock race t6300-6420):
//
//   T1 base:   ALPHA (2,2,2,0) (replace-by-blend-unit), paletted TEX0 (T8/T4 CLUT),
//              FBMSK 0 (full write), TEX1 nomip-filter fields (LCM 0, MMAG 1),
//              CLAMP 0 (repeat/repeat), ATE on with ATST ALWAYS (nominal only).
//   T3 shadow: ALPHA (1,0,0,2) ((Cd-Cs)*As), CT32/CT16 TEX0, FBMSK ff000000.
//   T2 light:  ALPHA (0,2,1,1) (Cs*Ad+Cd), CT32 TEX0, FBMSK ff000000.
//
// Only ST differs between the passes (0xc30 ST rewrite / 0xd40 UV set B
// re-kick); RGBAQ (incl. Q) match. The fused draw keeps T1's vertices plus the
// T3/T2 ST sets and evaluates all three stages in one PS_FUSE3 fragment run.
//
// Safety shape: classify the three passes by EXACT register template, require
// byte-equality on XYZ+RGBAQ and the index stream, and require equality (not a
// specific value) on every other draw input. Anything outside the template
// passes through unfused; the back-side verify step re-checks the
// trace/texture-dependent decisions and aborts to three normal draws on any
// surprise. UV/FOG vertex fields are ignored: the template pins FST=0/FGE=0,
// under which the shader never reads them.

#include "GS/GSRegs.h"

#include "common/Pcsx2Defs.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace GSTerrainFusePolicy
{

enum class Pass : u8
{
	None = 0,
	Base = 1, // T1
	Shadow = 2, // T3
	Light = 3, // T2
};

// Raw register words for one draw, filled by the caller from the draw's own
// context (each pass reads its own CTXT; the CTXT pattern itself is free).
struct DrawRegs
{
	u32 prim; // GIFRegPRIM low word (type/iip/tme/fge/abe/aa1/fst/ctxt/fix)
	u64 tex0;
	u32 tex1; // low 24 bits significant (high word is game garbage)
	u64 clamp;
	u64 alpha;
	u64 test;
	u64 frame;
	u64 zbuf;
	u64 texa;
	u64 texclut;
	u64 xyoffset;
	u64 scissor;
	u64 miptbp2;
	u64 tex2;
	u64 fogcol;
	u32 fba;
	u32 pabe;
	u32 colclamp;
	u32 dthe;
	u32 scanmsk;
	u64 dimx;
};

namespace detail
{
	__fi static bool IsTriangleType(u32 prim)
	{
		const u32 t = prim & 7u;
		return t == 3 || t == 4 || t == 5; // LIST/STRIP/FAN (GS_POINT=0 .. GS_SPRITE=6)
	}
	__fi static bool IsPaletted(u32 psm)
	{
		return psm == 19 || psm == 20 || psm == 27 || psm == 36 || psm == 44; // T8/T4/T8H/T4HL/T4HH
	}
	__fi static bool IsPlainColor(u32 psm)
	{
		return psm == 0 || psm == 1 || psm == 2 || psm == 10; // CT32/CT24/CT16/CT16S
	}
} // namespace detail

// Pass template shared by all three passes. Returns false for anything the
// fused shader cannot reproduce (fixed-function UV/fog/coverage paths,
// per-pixel blend enable, wrap/dither/scanmask output transforms).
__fi static bool MatchCommon(u32 prim, u64 test, u32 fba, u32 pabe, u32 colclamp, u32 dthe, u32 scanmsk)
{
	if (!detail::IsTriangleType(prim))
		return false;
	if (((prim >> 3) & 1u) != 1) // IIP (templated for stability; DECAL makes color moot)
		return false;
	if (((prim >> 4) & 1u) != 1) // TME
		return false;
	if (((prim >> 5) & 1u) != 0) // FGE
		return false;
	if (((prim >> 6) & 1u) != 1) // ABE
		return false;
	if (((prim >> 7) & 1u) != 0) // AA1
		return false;
	if (((prim >> 8) & 1u) != 0) // FST (STQ only; FST=1 uses the ti.xy path)
		return false;
	if (((prim >> 10) & 1u) != 0) // FIX
		return false;
	// TEST: ATE on, ATST ALWAYS (nominal), AFAIL KEEP-or-FB_ONLY (flows, moot when ALWAYS),
	// DATE off, ZTE on, ZTST GEQUAL. AREF/DATM flow through and are dead here.
	if (((test >> 0) & 1u) != 1) // ATE
		return false;
	if (((test >> 1) & 7u) != 1) // ATST ALWAYS
		return false;
	if (((test >> 14) & 1u) != 0) // DATE
		return false;
	if (((test >> 16) & 1u) != 1) // ZTE
		return false;
	if (((test >> 17) & 3u) != 2) // ZTST GEQUAL
		return false;
	if (fba != 0 || pabe != 0)
		return false;
	if (colclamp != 1 || dthe != 0 || scanmsk != 0)
		return false;
	return true;
}

// FRAME PSM must be 32-bit (CT32/CT24). This alone proves
// DetectTextureShuffleImpl returns None for the pass (its early exit on
// frame_psm.bpp != 16), so the fused draw can skip the T3/T2 verdicts.
__fi static bool MatchFramePSM(u64 frame)
{
	const u32 fpsm = static_cast<u32>((frame >> 24) & 0x3Fu);
	return fpsm == 0 || fpsm == 1;
}

__fi static Pass ClassifyPass(const DrawRegs& r)
{
	if (!MatchCommon(r.prim, r.test, r.fba, r.pabe, r.colclamp, r.dthe, r.scanmsk))
		return Pass::None;
	if (!MatchFramePSM(r.frame))
		return Pass::None;

	// GIFRegTEX0: TBP0 0-13, TBW 14-19, PSM 20-25, TW 26-29, TH 30-33, TCC 34, TFX 35-36.
	const u32 psm = static_cast<u32>((r.tex0 >> 20) & 0x3Fu);
	const u32 tcc = static_cast<u32>((r.tex0 >> 34) & 1u);
	const u32 tfx = static_cast<u32>((r.tex0 >> 35) & 3u);
	if (tcc != 1 || tfx != 0)
		return Pass::None;

	// TEX1 filter fields: bilinear mag (MMAG 1). Mipmap behaviour (MXL/MMIN/LCM/
	// MTBA/L/K) is unconstrained: the back-side verify replicates the per-pass LOD
	// decisions exactly and aborts on any surprise (self-checked against T1).
	const u32 mmag = (r.tex1 >> 5) & 1u;
	if (mmag != 1)
		return Pass::None;

	const u32 a = static_cast<u32>((r.alpha >> 0) & 3u);
	const u32 b = static_cast<u32>((r.alpha >> 2) & 3u);
	const u32 c = static_cast<u32>((r.alpha >> 4) & 3u);
	const u32 d = static_cast<u32>((r.alpha >> 6) & 3u);
	const u32 fbmsk = static_cast<u32>((r.frame >> 32) & 0xFFFFFFFFu);

	// T1 base: (0-0)*FIX+Cs, full write, paletted, repeat/repeat clamp.
	if (a == 2 && b == 2 && c == 2 && d == 0 && fbmsk == 0x00000000u && detail::IsPaletted(psm) &&
		r.clamp == 0)
		return Pass::Base;
	// T3 shadow: (Cd-Cs)*As, alpha preserved, plain color, clamp/clamp.
	if (a == 1 && b == 0 && c == 0 && d == 2 && fbmsk == 0xFF000000u && detail::IsPlainColor(psm) &&
		r.clamp == 5)
		return Pass::Shadow;
	// T2 light: (Cs-0)*Ad+Cd, alpha preserved, plain color, clamp/clamp.
	if (a == 0 && b == 2 && c == 1 && d == 1 && fbmsk == 0xFF000000u && detail::IsPlainColor(psm) &&
		r.clamp == 5)
		return Pass::Light;
	return Pass::None;
}

// Everything outside the per-pass template must be EQUAL across the triple (not
// a specific value): any game-state change still fuses, and any divergence
// safely refuses. TEX0/TEX1/CLAMP/ALPHA/MIPTBP1 vary by design (per-pass
// template + per-pass lookup); PRIM may differ only in the CTXT bit.
__fi static bool StatesCompatible(const DrawRegs& t1, const DrawRegs& t3, const DrawRegs& t2)
{
	if ((t1.prim & ~(1u << 9)) != (t3.prim & ~(1u << 9)) || (t1.prim & ~(1u << 9)) != (t2.prim & ~(1u << 9)))
		return false;
	if (t1.test != t3.test || t1.test != t2.test)
		return false;
	// FRAME: FBP/FBW/PSM (low word) must match; FBMSK (high word) is templated per pass.
	if (static_cast<u32>(t1.frame) != static_cast<u32>(t3.frame) ||
		static_cast<u32>(t1.frame) != static_cast<u32>(t2.frame))
		return false;
	// ZBUF: full equality incl. ZMSK (a pass-2/3-only Z write has no fused spelling).
	if (t1.zbuf != t3.zbuf || t1.zbuf != t2.zbuf)
		return false;
	if (t1.texa != t3.texa || t1.texa != t2.texa)
		return false;
	if (t1.texclut != t3.texclut || t1.texclut != t2.texclut)
		return false;
	if (t1.xyoffset != t3.xyoffset || t1.xyoffset != t2.xyoffset)
		return false;
	if (t1.scissor != t3.scissor || t1.scissor != t2.scissor)
		return false;
	if (t1.miptbp2 != t3.miptbp2 || t1.miptbp2 != t2.miptbp2)
		return false;
	if (t1.tex2 != t3.tex2 || t1.tex2 != t2.tex2)
		return false;
	if (t1.fogcol != t3.fogcol || t1.fogcol != t2.fogcol)
		return false;
	if (t1.dimx != t3.dimx || t1.dimx != t2.dimx)
		return false;
	return true;
}

// Vertex-stream identity: XYZ (bytes 16..24) + RGBAQ (bytes 8..16, incl. Q) per
// vertex, plus the raw index bytes and counts. ST is carried per pass (allowed
// to differ); UV/FOG are dead under the FST=0/FGE=0 template.
__fi static bool VertexStreamsMatch(
	const void* v0, const void* v1, u32 nverts, const void* i0, const void* i1, u32 nindices)
{
	if (nverts == 0)
		return false;
	const char* a = static_cast<const char*>(v0);
	const char* b = static_cast<const char*>(v1);
	for (u32 i = 0; i < nverts; i++)
	{
		if (std::memcmp(a + i * 32 + 8, b + i * 32 + 8, 8) != 0) // RGBAQ
			return false;
		if (std::memcmp(a + i * 32 + 16, b + i * 32 + 16, 8) != 0) // XYZ
			return false;
	}
	if (nindices > 0 && std::memcmp(i0, i1, static_cast<size_t>(nindices) * sizeof(u16)) != 0)
		return false;
	return true;
}

} // namespace GSTerrainFusePolicy
