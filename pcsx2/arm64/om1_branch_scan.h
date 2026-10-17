// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// OM1 recorder branch-word classifier: single-word A64 branch decode for the
// chunk safety scan (microVU_OM1-arm64.inl safetyNets). Masks live here, next
// to VIXL's constants, so the scan and its unit test share one definition.
#pragma once

#include "vixl/aarch64/constants-aarch64.h"

#include <cstdint>

namespace om1
{
enum class BranchKind : uint8_t
{
    None,
    B,
    BL,
    BCond,
    CBZ,
    CBNZ,
    TBZ,
    TBNZ,
    ADR
};

constexpr BranchKind classifyBranch(uint32_t w)
{
    using namespace vixl::aarch64;
    if ((w & 0xFC000000u) == 0x14000000u)
        return BranchKind::B;
    if ((w & 0xFC000000u) == 0x94000000u)
        return BranchKind::BL;
    if ((w & 0xFF000000u) == static_cast<uint32_t>(ConditionalBranchFixed))
        return BranchKind::BCond;
    if ((w & 0x7F000000u) == 0x34000000u)
        return BranchKind::CBZ;
    if ((w & 0x7F000000u) == 0x35000000u)
        return BranchKind::CBNZ;
    // R4: TBZ/TBNZ are TestBranchFixed 0x36000000 (+0x01000000 for TBNZ)
    // under TestBranchMask. The old 0x68000000/0x69000000 masks match LDR
    // words instead and never match a real TBZ/TBNZ.
    if ((w & TestBranchMask) == TBZ)
        return BranchKind::TBZ;
    if ((w & TestBranchMask) == TBNZ)
        return BranchKind::TBNZ;
    if ((w & 0x9F000000u) == 0x10000000u)
        return BranchKind::ADR;
    return BranchKind::None;
}

} // namespace om1
