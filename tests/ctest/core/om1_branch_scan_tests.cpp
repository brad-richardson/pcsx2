// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Unit test for the OM1 recorder branch-word classifier (R4). A TBZ/TBNZ
// word is 0x36000000/0x37000000 under TestBranchMask (VIXL); the shipped
// 0x68000000/0x69000000 masks never match one, so an escaping TBZ/TBNZ
// slipped past the chunk safety scan. Real vixl-emitted words below pin
// the correct classification, including words the old masks misclassified.
#include "arm64/om1_branch_scan.h"

#include <gtest/gtest.h>

using om1::BranchKind;
using om1::classifyBranch;

TEST(Om1BranchScan, DecodesUnconditional)
{
    EXPECT_EQ(classifyBranch(0x14000005u), BranchKind::B); // b .+20
    EXPECT_EQ(classifyBranch(0x97FFFFE0u), BranchKind::BL); // bl .-128
}

TEST(Om1BranchScan, DecodesConditional)
{
    EXPECT_EQ(classifyBranch(0x54000040u), BranchKind::BCond); // b.eq .+8
    EXPECT_EQ(classifyBranch(0x54000061u), BranchKind::BCond); // b.ne .+12
}

TEST(Om1BranchScan, DecodesCompareBranch)
{
    EXPECT_EQ(classifyBranch(0x34000020u), BranchKind::CBZ); // cbz w0, .+8
    EXPECT_EQ(classifyBranch(0xB5000081u), BranchKind::CBNZ); // cbnz x1, .+16 (64-bit form)
}

TEST(Om1BranchScan, DecodesTestBranch)
{
    EXPECT_EQ(classifyBranch(0x36000000u), BranchKind::TBZ); // tbz w0, #0, .+0
    EXPECT_EQ(classifyBranch(0x36007FE0u), BranchKind::TBZ); // tbz w0, #0, .+4092
    EXPECT_EQ(classifyBranch(0x37000021u), BranchKind::TBNZ); // tbnz w1, #0, .+4
    EXPECT_EQ(classifyBranch(0xB7000042u), BranchKind::TBNZ); // tbnz x2, #0, .+8 (64-bit form)
}

TEST(Om1BranchScan, DecodesAdr)
{
    EXPECT_EQ(classifyBranch(0x10000000u), BranchKind::ADR); // adr x0, .+0
}

TEST(Om1BranchScan, RejectsNonBranches)
{
    EXPECT_EQ(classifyBranch(0xD503201Fu), BranchKind::None); // nop
    EXPECT_EQ(classifyBranch(0xD65F03C0u), BranchKind::None); // ret
    EXPECT_EQ(classifyBranch(0x68000000u), BranchKind::None); // ldr (the old bogus TBZ mask)
    EXPECT_EQ(classifyBranch(0x69192820u), BranchKind::None); // ldr (the old bogus TBNZ mask)
}
