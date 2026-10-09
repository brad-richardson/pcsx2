// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"

// Rewriting a lopsided ONE_MINUS_SRC1_* blend to its SRC1_* twin, with the fragment shader
// writing 1-f to the second output instead of f.
//
// Our Turnip misrenders ONE_MINUS_SRC1_* factors in sysmem passes on A830 (opaque glyph boxes;
// TU3 class B) while SRC1_* factors are exact in sysmem, so MR1 0004 forces every pass containing
// an inverted-src1 draw into GMEM. A 1-draw GMEM pass costs 460-590 us against ~57 us for a 1-draw
// sysmem pass (GPF1), so emitting the uninverted factor with a complemented second output lets
// those passes take the normal sysmem road.
//
// The blend unit computes Cd*(1-f) either way; the only difference is WHERE the 1-f comes from
// (the fixed-function complement vs an fp32 shader `1.0 - f`), which is <= 1 LSB per channel on
// the validated captures (OMS1).
//
// Only lopsided states qualify: a state reading both f (SRC1_*) and 1-f (INV_SRC1_*) from one
// output (plain MIX1/MIX3, SIMPLE_RGB_ONLY alpha) has no single-output spelling without an
// inverted factor and is refused; those passes keep the 0004 GMEM road.
//
// Pure functions so the no-change case (knob off takes identical decisions) can be tested
// off-device.
namespace GSInvSrc1Policy
{
	/// The uninverted twin of an inverted-src1 blend factor. Every other factor is unchanged.
	static constexpr u8 RemapFactor(u8 factor)
	{
		switch (factor)
		{
			case GSDevice::INV_SRC1_COLOR:
				return GSDevice::SRC1_COLOR;
			case GSDevice::INV_SRC1_ALPHA:
				return GSDevice::SRC1_ALPHA;
			default:
				return factor;
		}
	}

	/// Does any of the four factors read the second output inverted?
	static constexpr bool ReadsInvSrc1(const GSHWDrawConfig::BlendState& bs)
	{
		return bs.src_factor == GSDevice::INV_SRC1_COLOR || bs.dst_factor == GSDevice::INV_SRC1_COLOR ||
		       bs.src_factor_alpha == GSDevice::INV_SRC1_ALPHA || bs.dst_factor_alpha == GSDevice::INV_SRC1_ALPHA;
	}

	/// Does any of the four factors read the second output uninverted?
	static constexpr bool ReadsPlainSrc1(const GSHWDrawConfig::BlendState& bs)
	{
		return bs.src_factor == GSDevice::SRC1_COLOR || bs.dst_factor == GSDevice::SRC1_COLOR ||
		       bs.src_factor_alpha == GSDevice::SRC1_ALPHA || bs.dst_factor_alpha == GSDevice::SRC1_ALPHA;
	}

	/// The blend state with inverted-src1 factors moved to their uninverted twins. The blend
	/// constant rides along untouched: this rewrite never moves a constant factor.
	static constexpr GSHWDrawConfig::BlendState RemapToSrc1(const GSHWDrawConfig::BlendState& bs)
	{
		return GSHWDrawConfig::BlendState(bs.enable, RemapFactor(bs.src_factor), RemapFactor(bs.dst_factor), bs.op,
			RemapFactor(bs.src_factor_alpha), RemapFactor(bs.dst_factor_alpha), bs.constant_enable, bs.constant);
	}

	/// Pixel-shader selector state the decision reads, beyond the blend state.
	struct DrawInputs
	{
		/// GE1_ADRENO_SRC1_REWRITE=1 reached this backend (a Vulkan-only features bit).
		bool inv_src1_rewrite = false;
		/// The device has a second fragment output to blend from at all.
		bool dual_source_blend = true;
		/// This draw's alpha-test mode is SIMPLE_RGB_ONLY. EmulateAlphaTestSecondPass runs after
		/// EmulateBlending and gives such a draw (SRC1_ALPHA, INV_SRC1_ALPHA) alpha factors, which
		/// would read the complemented output uninverted. Nothing can become SIMPLE_RGB_ONLY later
		/// (only EmulateAlphaTest sets it), so refusing here closes the hazard completely.
		bool simple_rgb_only = false;
		/// PABE reads the second output's alpha as the source alpha to decide per pixel whether to
		/// blend; the complement would flip its decision.
		bool pabe = false;
		/// The blend_factor_in_alpha road (no dual source) already uses the outputs for something
		/// else; mutually exclusive with this rewrite, refused for the same reason as PABE.
		bool blend_factor_in_alpha = false;
		/// The blend multi-pass second draw reads the second output, and both passes share one
		/// pixel shader.
		bool multi_pass_reads_second_output = false;
	};

	/// May this draw's inverted-src1 factors travel uninverted with a complemented second output?
	///
	/// Guards: the backend asked for the rewrite and has a second output; the draw is blended and
	/// its state is lopsided (an inverted-src1 factor with no plain-src1 reader anywhere in the
	/// four factors, so complementing the whole second output changes nothing else the blend unit
	/// reads); no later pass adds a plain-src1 reader (SIMPLE_RGB_ONLY alpha, multi-pass second
	/// draw); and nothing else owns the second output's value (PABE, blend_factor_in_alpha).
	static constexpr bool CanRewriteInvSrc1(const GSHWDrawConfig::BlendState& bs, const DrawInputs& in)
	{
		return in.inv_src1_rewrite && in.dual_source_blend && bs.enable && ReadsInvSrc1(bs) &&
		       !ReadsPlainSrc1(bs) && !in.simple_rgb_only && !in.pabe && !in.blend_factor_in_alpha &&
		       !in.multi_pass_reads_second_output;
	}
} // namespace GSInvSrc1Policy
