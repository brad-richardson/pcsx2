// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <cstddef>
#include <cstring>

// NRS1: the compact native record. Buddy of the runtime contract in
// PS2Recomp ps2xRuntime/include/runtime/gs/ge1_gs_api.h (Ge1CompactVertex,
// magic, layout): mirrored here because the vendor builds standalone. Any
// drift refuses loudly: magic + exact size are checked at ingest,
// fail-closed, and the field offsets are pinned below.
#define GE1_COMPACT_RECORD_MAGIC_LO 0x00002A4E52430000ull
#define GE1_COMPACT_RECORD_MAGIC_HI 0x5245434F52440002ull

struct Ge1CompactVertex
{
	u32 S, T;   // ST.w0, ST.w1 (raw bits)
	u32 RGBA;   // R | G<<8 | B<<16 | A<<24 (low bytes of the RGBAQ words)
	u32 Q;      // ST.w2 (raw bits; Q==+0.0 -> FLT_MIN applies at kick)
	u32 X, Y;   // XYZF2.w0, XYZF2.w1 verbatim (the kick reads the low halves)
	u32 Z;      // XYZF2.w2 verbatim
	u32 W3;     // XYZF2.w3 verbatim (F + ADC)
};

static_assert(sizeof(Ge1CompactVertex) == 32);
static_assert(offsetof(Ge1CompactVertex, S) == 0);
static_assert(offsetof(Ge1CompactVertex, T) == 4);
static_assert(offsetof(Ge1CompactVertex, RGBA) == 8);
static_assert(offsetof(Ge1CompactVertex, Q) == 12);
static_assert(offsetof(Ge1CompactVertex, X) == 16);
static_assert(offsetof(Ge1CompactVertex, Y) == 20);
static_assert(offsetof(Ge1CompactVertex, Z) == 24);
static_assert(offsetof(Ge1CompactVertex, W3) == 28);

// Calls fn(packet, size) per compact packet in order; false on a malformed
// record. Packet sizes are bounds-checked against the record; the tag shape
// (PACKED, PRE, NREG 3, {STQ, RGBAQ, XYZF2}) is the caller's check, via
// GIFPath::SetTag, because only it knows the path state.
template <class Fn>
inline bool ge1_compact_record_for_each(const u8* bytes, u32 size, Fn&& fn)
{
	u64 lo = 0, hi = 0;
	if (!bytes || size < 32u || (size & 15u))
		return false;
	std::memcpy(&lo, bytes, 8);
	std::memcpy(&hi, bytes + 8, 8);
	if (lo != GE1_COMPACT_RECORD_MAGIC_LO || hi != GE1_COMPACT_RECORD_MAGIC_HI)
		return false;
	u32 count = 0;
	std::memcpy(&count, bytes + 16, 4);
	const u32 table = (8u + 4u * count + 15u) & ~15u;
	if (count == 0 || count > 64u || 16u + table > size)
		return false;
	u32 off = 16u + table;
	for (u32 i = 0; i < count; ++i)
	{
		u32 n = 0;
		std::memcpy(&n, bytes + 24 + 4 * i, 4);
		if (n < 16u || (n & 15u) || n > size - off || (n - 16u) % 32u)
			return false;
		if (!fn(bytes + off, n))
			return false;
		off += n;
	}
	return off == size;
}

// Expands one compact packet to the GIF packet bytes the kick is equivalent
// to (the tag verbatim, then ST/RGBAQ/XYZF2 regs; ST.w3 and the RGBAQ upper
// bytes expand as zero, which no kick path reads). False on a malformed
// packet (out untouched).
inline bool ge1_compact_expand_packet(const u8* pkt, u32 size, u8* out, u32 outSize)
{
	if (!pkt || !out || size < 16u || ((size - 16u) % 32u))
		return false;
	const u32 n = (size - 16u) / 32u;
	if (outSize < 16u + 48u * n)
		return false;
	std::memcpy(out, pkt, 16);
	for (u32 i = 0; i < n; ++i)
	{
		Ge1CompactVertex v;
		std::memcpy(&v, pkt + 16 + 32u * i, 32);
		u32* reg = reinterpret_cast<u32*>(out + 16 + 48u * i);
		reg[0] = v.S;
		reg[1] = v.T;
		reg[2] = v.Q;
		reg[3] = 0;
		reg[4] = v.RGBA & 0xffu;
		reg[5] = (v.RGBA >> 8) & 0xffu;
		reg[6] = (v.RGBA >> 16) & 0xffu;
		reg[7] = (v.RGBA >> 24) & 0xffu;
		reg[8] = v.X;
		reg[9] = v.Y;
		reg[10] = v.Z;
		reg[11] = v.W3;
	}
	return true;
}
