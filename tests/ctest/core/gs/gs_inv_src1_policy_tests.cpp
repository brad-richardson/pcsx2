// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// Pins the inverted-src1 rewrite (GS/Renderers/Common/GSInvSrc1Policy.h): where the backend asked
// for it (GE1_ADRENO_SRC1_REWRITE=1), a draw whose blend state is lopsided -- an INV_SRC1_* factor
// with no plain SRC1_* reader in any of the four factors -- emits the uninverted factor with the
// fragment shader writing 1-f to the second output instead of f. The blend unit computes Cd*(1-f)
// either way; our Turnip renders SRC1_* exactly in sysmem while MR1 0004 forces INV_SRC1_* passes
// into GMEM (OMS1).
//
// Two halves. The remap itself is checked against the blend table: the lopsided rows (2000, 2101,
// and the mix arm's ONE/dst form) rewrite, and the remap touches nothing but the two inverted
// factors, so a refused state is unaltered even if it were applied by mistake.
//
// The other half is the refusals, and it is the half that matters. Every draw without the knob
// must take byte-identical decisions, and a conflict state (plain MIX1/MIX3, SIMPLE_RGB_ONLY
// alpha) has no single-output spelling without an inverted factor, so each guard is named and
// pinned: what makes the state not lopsided, and what else owns the second output's value.
//
// Rides gs_vertex_tests -- the policy is header-only constexpr, so it needs no extra linkage.

#include "GS/Renderers/Common/GSInvSrc1Policy.h"

#include <gtest/gtest.h>

namespace
{
	using BlendState = GSHWDrawConfig::BlendState;

	// The blend table index the renderer computes, from the four ALPHA register fields.
	constexpr u32 BlendIndex(u32 a, u32 b, u32 c, u32 d) { return ((a * 3 + b) * 3 + c) * 3 + d; }

	// The state GSRendererHW::EmulateBlending emits for a hardware-blended draw: the table's two
	// colour factors with ONE/ZERO on alpha.
	BlendState StateFor(u32 a, u32 b, u32 c, u32 d)
	{
		const HWBlend blend = GSDevice::GetBlend(BlendIndex(a, b, c, d));
		return BlendState(true, blend.src, blend.dst, blend.op, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0);
	}

	// A backend with the rewrite knob on, on a plain draw with nothing else claiming the second
	// output.
	GSInvSrc1Policy::DrawInputs RewriteOn()
	{
		GSInvSrc1Policy::DrawInputs in;
		in.inv_src1_rewrite = true;
		in.dual_source_blend = true;
		return in;
	}
} // namespace

// SSX 3's score/HUD mix draw: the blend-mix arm replaces the source factor with ONE and does that
// multiply in the shader, so the MIX1 equation reaches the blender as ONE / INV_SRC1_COLOR. This
// is the draw the whole change exists for.
TEST(GSInvSrc1Policy, HudMixRewritesToSrc1)
{
	const BlendState mix(true, GSDevice::CONST_ONE, GSDevice::INV_SRC1_COLOR, GSDevice::OP_ADD,
		GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0);
	EXPECT_TRUE(GSInvSrc1Policy::CanRewriteInvSrc1(mix, RewriteOn()));

	const BlendState after = GSInvSrc1Policy::RemapToSrc1(mix);
	EXPECT_EQ(after.src_factor, GSDevice::CONST_ONE);
	EXPECT_EQ(after.dst_factor, GSDevice::SRC1_COLOR);
	EXPECT_EQ(after.src_factor_alpha, GSDevice::CONST_ONE);
	EXPECT_EQ(after.dst_factor_alpha, GSDevice::CONST_ZERO);
	EXPECT_EQ(after.op, mix.op);
	EXPECT_TRUE(after.enable);
	// The blend constant rides along untouched: this rewrite never moves a constant factor.
	EXPECT_EQ(after.constant_enable, mix.constant_enable);
	EXPECT_EQ(after.constant, mix.constant);
}

// The blend table is its own oracle. Rows 2000 and 2101 carry an inverted-src1 factor with no
// plain-src1 reader, so they rewrite; rows 0101 (MIX1) and 1000 (MIX3) read both f and 1-f from
// the one output, so they are refused and keep the GMEM road.
TEST(GSInvSrc1Policy, BlendTableRowsSplitOnLopsidedness)
{
	const BlendState row2000 = StateFor(2, 0, 0, 0);
	ASSERT_EQ(row2000.src_factor, GSDevice::INV_SRC1_COLOR);
	ASSERT_EQ(row2000.dst_factor, GSDevice::CONST_ZERO);
	EXPECT_TRUE(GSInvSrc1Policy::CanRewriteInvSrc1(row2000, RewriteOn()));
	EXPECT_EQ(GSInvSrc1Policy::RemapToSrc1(row2000).src_factor, GSDevice::SRC1_COLOR);

	const BlendState row2101 = StateFor(2, 1, 0, 1);
	ASSERT_EQ(row2101.src_factor, GSDevice::CONST_ZERO);
	ASSERT_EQ(row2101.dst_factor, GSDevice::INV_SRC1_COLOR);
	EXPECT_TRUE(GSInvSrc1Policy::CanRewriteInvSrc1(row2101, RewriteOn()));

	const BlendState mix1 = StateFor(0, 1, 0, 1);
	ASSERT_EQ(mix1.src_factor, GSDevice::SRC1_COLOR);
	ASSERT_EQ(mix1.dst_factor, GSDevice::INV_SRC1_COLOR);
	EXPECT_FALSE(GSInvSrc1Policy::CanRewriteInvSrc1(mix1, RewriteOn()));

	const BlendState mix3 = StateFor(1, 0, 0, 0);
	ASSERT_EQ(mix3.src_factor, GSDevice::INV_SRC1_COLOR);
	ASSERT_EQ(mix3.dst_factor, GSDevice::SRC1_COLOR);
	EXPECT_FALSE(GSInvSrc1Policy::CanRewriteInvSrc1(mix3, RewriteOn()));
}

// The SIMPLE_RGB_ONLY second-pass setup gives the draw (SRC1_ALPHA, INV_SRC1_ALPHA) alpha factors,
// which read the second output both ways at once: the same conflict as MIX1, on alpha.
TEST(GSInvSrc1Policy, RgbOnlyAlphaIsAConflict)
{
	const BlendState rgb_only(true, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, GSDevice::OP_ADD,
		GSDevice::SRC1_ALPHA, GSDevice::INV_SRC1_ALPHA, false, 0);
	EXPECT_FALSE(GSInvSrc1Policy::CanRewriteInvSrc1(rgb_only, RewriteOn()));
}

// Everything that is not an inverted-src1 factor comes through the remap untouched, so a state the
// policy declines is a state the policy cannot alter even if it were applied by mistake.
TEST(GSInvSrc1Policy, RemapTouchesNothingElse)
{
	for (u8 f = 0; f <= GSDevice::CONST_ZERO; f++)
	{
		if (f == GSDevice::INV_SRC1_COLOR || f == GSDevice::INV_SRC1_ALPHA)
			continue;
		EXPECT_EQ(GSInvSrc1Policy::RemapFactor(f), f) << "factor " << static_cast<u32>(f);
	}

	// A state with no inverted factor at all is identical after the remap: the Ad form of
	// the equation (DST_ALPHA / INV_DST_ALPHA reads neither output). Conflict states like MIX1
	// are refused by CanRewriteInvSrc1, so the remap is never applied to them.
	const BlendState ad_row = StateFor(0, 1, 1, 1);
	const BlendState after = GSInvSrc1Policy::RemapToSrc1(ad_row);
	EXPECT_EQ(after.src_factor, ad_row.src_factor);
	EXPECT_EQ(after.dst_factor, ad_row.dst_factor);
	EXPECT_EQ(after.src_factor_alpha, ad_row.src_factor_alpha);
	EXPECT_EQ(after.dst_factor_alpha, ad_row.dst_factor_alpha);
}

// The refusals. Each of these leaves the blend path exactly as it is today.
TEST(GSInvSrc1Policy, RefusedWithoutTheKnob)
{
	const BlendState mix(true, GSDevice::CONST_ONE, GSDevice::INV_SRC1_COLOR, GSDevice::OP_ADD,
		GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0);

	GSInvSrc1Policy::DrawInputs in = RewriteOn();
	in.inv_src1_rewrite = false;
	EXPECT_FALSE(GSInvSrc1Policy::CanRewriteInvSrc1(mix, in));
}

TEST(GSInvSrc1Policy, RefusedWithoutASecondOutput)
{
	const BlendState mix(true, GSDevice::CONST_ONE, GSDevice::INV_SRC1_COLOR, GSDevice::OP_ADD,
		GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0);

	GSInvSrc1Policy::DrawInputs in = RewriteOn();
	in.dual_source_blend = false;
	EXPECT_FALSE(GSInvSrc1Policy::CanRewriteInvSrc1(mix, in));
}

TEST(GSInvSrc1Policy, RefusedWithNoInvertedFactor)
{
	const BlendState plain(true, GSDevice::CONST_ONE, GSDevice::CONST_ZERO, GSDevice::OP_ADD,
		GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0);
	EXPECT_FALSE(GSInvSrc1Policy::CanRewriteInvSrc1(plain, RewriteOn()));

	// Nor does a disabled blend state, even one still carrying an inverted factor.
	const BlendState disabled(false, GSDevice::CONST_ONE, GSDevice::INV_SRC1_COLOR, GSDevice::OP_ADD,
		GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0);
	EXPECT_FALSE(GSInvSrc1Policy::CanRewriteInvSrc1(disabled, RewriteOn()));
}

// SIMPLE_RGB_ONLY gains (SRC1_ALPHA, INV_SRC1_ALPHA) alpha factors after EmulateBlending, which
// would read the complemented output uninverted.
TEST(GSInvSrc1Policy, RefusedForSimpleRgbOnly)
{
	const BlendState mix(true, GSDevice::CONST_ONE, GSDevice::INV_SRC1_COLOR, GSDevice::OP_ADD,
		GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0);

	GSInvSrc1Policy::DrawInputs in = RewriteOn();
	in.simple_rgb_only = true;
	EXPECT_FALSE(GSInvSrc1Policy::CanRewriteInvSrc1(mix, in));
}

// The second output is already carrying something. Every one of these would read a complemented
// value out of it if the rewrite went ahead, and get a different number.
TEST(GSInvSrc1Policy, RefusedWhenTheSecondOutputIsSpokenFor)
{
	const BlendState mix(true, GSDevice::CONST_ONE, GSDevice::INV_SRC1_COLOR, GSDevice::OP_ADD,
		GSDevice::CONST_ONE, GSDevice::CONST_ZERO, false, 0);

	// The blend multi-pass second draw shares the pixel shader with the first, so if it reads the
	// second output, the complement would land on it too.
	GSInvSrc1Policy::DrawInputs multi = RewriteOn();
	multi.multi_pass_reads_second_output = true;
	EXPECT_FALSE(GSInvSrc1Policy::CanRewriteInvSrc1(mix, multi));

	// PABE reads the second output's alpha as the source alpha.
	GSInvSrc1Policy::DrawInputs pabe = RewriteOn();
	pabe.pabe = true;
	EXPECT_FALSE(GSInvSrc1Policy::CanRewriteInvSrc1(mix, pabe));

	// And the no-dual-source substitution is already using the factor for something else.
	GSInvSrc1Policy::DrawInputs in_alpha = RewriteOn();
	in_alpha.blend_factor_in_alpha = true;
	EXPECT_FALSE(GSInvSrc1Policy::CanRewriteInvSrc1(mix, in_alpha));
}
