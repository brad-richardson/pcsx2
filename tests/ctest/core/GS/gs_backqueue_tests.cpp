// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Unit suite for the GV-7 front->back SPSC ring (GS/GSBackQueue.h): capacity /
// FIFO / wraparound behavior single-threaded, tag round-trips through the
// tagged record slots, and a two-thread producer/consumer stress that checks
// every value crosses in order exactly once.

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <thread>

#include "GS/GSBackQueue.h"

using namespace GSBackQueue;

TEST(GsBackQueue, FifoAndCapacity)
{
	SpscRing<u32, 8> ring;

	EXPECT_TRUE(ring.IsEmpty());
	EXPECT_EQ(ring.Peek(), nullptr);

	// Fill to capacity.
	for (u32 i = 0; i < 8; i++)
	{
		u32* slot = ring.BeginPush();
		ASSERT_NE(slot, nullptr);
		*slot = 100 + i;
		ring.CommitPush();
	}
	EXPECT_EQ(ring.Size(), 8u);
	EXPECT_EQ(ring.BeginPush(), nullptr); // full -> backpressure

	// Pop one, a slot frees up.
	ASSERT_NE(ring.Peek(), nullptr);
	EXPECT_EQ(*ring.Peek(), 100u);
	ring.Pop();
	u32* slot = ring.BeginPush();
	ASSERT_NE(slot, nullptr);
	*slot = 108;
	ring.CommitPush();

	// Drain: strict FIFO.
	for (u32 i = 1; i <= 8; i++)
	{
		u32* p = ring.Peek();
		ASSERT_NE(p, nullptr);
		EXPECT_EQ(*p, 100 + i);
		ring.Pop();
	}
	EXPECT_TRUE(ring.IsEmpty());
}

TEST(GsBackQueue, WraparoundManyTimes)
{
	// Push/pop far past the slot count so the cursors lap the ring repeatedly.
	SpscRing<u32, 4> ring;
	for (u32 i = 0; i < 1000; i++)
	{
		u32* slot = ring.BeginPush();
		ASSERT_NE(slot, nullptr);
		*slot = i;
		ring.CommitPush();

		u32* p = ring.Peek();
		ASSERT_NE(p, nullptr);
		EXPECT_EQ(*p, i);
		ring.Pop();
	}
	EXPECT_TRUE(ring.IsEmpty());
}

TEST(GsBackQueue, LimitBelowCapacity)
{
	// GW3: a limit below the slot count reports full at the limit and keeps
	// FIFO order across wraparound (indices still mask by the slot count).
	SpscRing<u32, 16> ring;
	ring.SetLimit(4);
	for (u32 i = 0; i < 1000; i++)
	{
		while (ring.Size() < 4)
		{
			u32* slot = ring.BeginPush();
			ASSERT_NE(slot, nullptr);
			*slot = i * 4 + ring.Size();
			ring.CommitPush();
		}
		EXPECT_EQ(ring.BeginPush(), nullptr); // full at the limit
		for (u32 k = 0; k < 4; k++)
		{
			u32* p = ring.Peek();
			ASSERT_NE(p, nullptr);
			EXPECT_EQ(*p, i * 4 + k);
			ring.Pop();
		}
	}
	EXPECT_TRUE(ring.IsEmpty());

	ring.SetLimit(0); // out of range -> full capacity
	for (u32 i = 0; i < 16; i++)
	{
		ASSERT_NE(ring.BeginPush(), nullptr);
		ring.CommitPush();
	}
	EXPECT_EQ(ring.BeginPush(), nullptr);
}

TEST(GsBackQueue, ChannelCapsScale)
{
	// GW3 knob (GE1_BACKQ_CAPS): stock caps by default and for any value but 2/4.
	Channel chan;
	EXPECT_EQ(chan.draw_cap, kBaseDrawNodes);
	EXPECT_EQ(chan.payload_cap, kBasePayloadNodes);
	const u32 scales[] = {0, 1, 2, 3, 4, 8};
	for (u32 scale : scales)
	{
		Channel c;
		c.SetCapsScale(scale);
		const u32 want = (scale == 2 || scale == 4) ? scale : 1;
		EXPECT_EQ(c.draw_cap, kBaseDrawNodes * want);
		EXPECT_EQ(c.payload_cap, kBasePayloadNodes * want);
		EXPECT_LE(c.draw_cap, Channel::kMaxDrawNodes);
		EXPECT_LE(c.payload_cap, Channel::kMaxPayloadNodes);
		u32 pushed = 0;
		while (c.ring.BeginPush())
		{
			c.ring.CommitPush();
			pushed++;
		}
		EXPECT_EQ(pushed, kBaseRecords * want);
	}
}

TEST(GsBackQueue, RecordSlotTagRoundTrip)
{
	RecordRing ring;

	{
		RecordSlot* slot = ring.BeginPush();
		ASSERT_NE(slot, nullptr);
		slot->type = RecordType::Vsync;
		VsyncRecord* rec = slot->As<VsyncRecord>();
		rec->field = 1;
		rec->registers_written = true;
		rec->idle_frame = false;
		ring.CommitPush();
	}
	{
		RecordSlot* slot = ring.BeginPush();
		ASSERT_NE(slot, nullptr);
		slot->type = RecordType::Transfer;
		TransferRecord* rec = slot->As<TransferRecord>();
		std::memset(rec, 0, sizeof(*rec));
		rec->len = 0x1234;
		rec->draw_serial = 0xdeadbeefcafeull;
		rec->first_slice = true;
		ring.CommitPush();
	}
	{
		RecordSlot* slot = ring.BeginPush();
		ASSERT_NE(slot, nullptr);
		slot->type = RecordType::Draw;
		DrawRecord* rec = slot->As<DrawRecord>();
		std::memset(rec, 0, sizeof(*rec));
		rec->draw_serial = 42;
		rec->flush_reason = 7;
		ring.CommitPush();
	}

	EXPECT_EQ(ring.Size(), 3u);

	const RecordSlot* s = ring.Peek();
	ASSERT_NE(s, nullptr);
	EXPECT_EQ(s->type, RecordType::Vsync);
	EXPECT_EQ(s->As<VsyncRecord>()->field, 1u);
	EXPECT_TRUE(s->As<VsyncRecord>()->registers_written);
	ring.Pop();

	s = ring.Peek();
	ASSERT_NE(s, nullptr);
	EXPECT_EQ(s->type, RecordType::Transfer);
	EXPECT_EQ(s->As<TransferRecord>()->len, 0x1234);
	EXPECT_EQ(s->As<TransferRecord>()->draw_serial, 0xdeadbeefcafeull);
	EXPECT_TRUE(s->As<TransferRecord>()->first_slice);
	ring.Pop();

	s = ring.Peek();
	ASSERT_NE(s, nullptr);
	EXPECT_EQ(s->type, RecordType::Draw);
	EXPECT_EQ(s->As<DrawRecord>()->draw_serial, 42u);
	EXPECT_EQ(s->As<DrawRecord>()->flush_reason, 7);
	ring.Pop();

	EXPECT_TRUE(ring.IsEmpty());
}

TEST(GsBackQueue, ThreadedStress)
{
	// Two real threads across a deliberately small ring so full/empty edges and
	// wraparound get hammered. The consumer checks strict sequence order; both
	// sides spin (never sleep), so the test also proves forward progress under
	// pure backpressure.
	constexpr u64 kValues = 1'000'000;
	SpscRing<u64, 64> ring;
	std::atomic<u64> bad{0};

	std::thread consumer([&ring, &bad]() {
		u64 expected = 0;
		while (expected < kValues)
		{
			u64* p = ring.Peek();
			if (!p)
			{
				std::this_thread::yield();
				continue;
			}
			if (*p != expected)
				bad.fetch_add(1, std::memory_order_relaxed);
			expected++;
			ring.Pop();
		}
	});

	for (u64 i = 0; i < kValues; i++)
	{
		u64* slot;
		while (!(slot = ring.BeginPush()))
			std::this_thread::yield();
		*slot = i;
		ring.CommitPush();
	}

	consumer.join();
	EXPECT_EQ(bad.load(), 0u);
	EXPECT_TRUE(ring.IsEmpty());
}
