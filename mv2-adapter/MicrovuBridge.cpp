// SPDX-License-Identifier: GPL-3.0-or-later
// MV2: private ARMSX2 core, exported through ps2_microvu_api.h only.
#include "ps2_microvu_api.h"

#include "RecompilerTestEnvironment.h"
#include "Config.h"
#include "Gif_Unit.h"
#include "VU.h"
#include "VUmicro.h"
#include "common/FPControl.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace {
bool s_ready = false;
// MP1 L1: PS2X_MICROVU_BRIDGE_LEAN=0 keeps the pre-MP1 byte-by-byte code diff
// (read once at init; the runtime reads the same knob for shared data).
bool s_leanDiff = true;

// MP1 L1: first differing byte of a/b (n if none). Chunked memcmp (the
// libc's vector compare) instead of a byte loop; same result.
uint32_t firstDiff(const uint8_t* a, const uint8_t* b, uint32_t n)
{
    constexpr uint32_t kChunk = 256u;
    uint32_t i = 0;
    while (i + kChunk <= n && std::memcmp(a + i, b + i, kChunk) == 0)
        i += kChunk;
    for (; i < n; ++i)
        if (a[i] != b[i])
            return i;
    return n;
}

// MP1 L1: one past the last differing byte of a/b in [lo, n) (lo if none).
uint32_t lastDiffEnd(const uint8_t* a, const uint8_t* b, uint32_t n, uint32_t lo)
{
    constexpr uint32_t kChunk = 256u;
    uint32_t j = n;
    while (j >= lo + kChunk && std::memcmp(a + j - kChunk, b + j - kChunk, kChunk) == 0)
        j -= kChunk;
    while (j > lo && a[j - 1] == b[j - 1])
        --j;
    return j;
}
bool s_seeded = false;
uint64_t s_code_generation = std::numeric_limits<uint64_t>::max();
std::vector<uint8_t> s_path1;
ps2x_microvu_path1_fn s_path1_fn = nullptr;
void* s_path1_opaque = nullptr;

void completePath1()
{
    if (!s_path1.empty() && s_path1_fn)
        s_path1_fn(s_path1_opaque, s_path1.data(), static_cast<uint32_t>(s_path1.size()));
    s_path1.clear();
}

void seed(const ps2x_microvu_state& state)
{
    for (unsigned r = 0; r < 32; ++r)
        for (unsigned lane = 0; lane < 4; ++lane)
            VU1.VF[r].UL[lane] = state.vf[r][lane];
    for (unsigned r = 0; r < 16; ++r)
        VU1.VI[r].UL = state.vi[r];
    for (unsigned lane = 0; lane < 4; ++lane)
        VU1.ACC.UL[lane] = state.acc[lane];
    VU1.q.UL = VU1.VI[REG_Q].UL = state.q;
    VU1.p.UL = VU1.VI[REG_P].UL = state.p;
    VU1.VI[REG_I].UL = state.i;
    VU1.VI[REG_R].UL = state.r;
    VU1.VI[REG_MAC_FLAG].UL = state.mac;
    VU1.VI[REG_CLIP_FLAG].UL = state.clip;
    VU1.VI[REG_STATUS_FLAG].UL = state.status;
    for (unsigned k = 0; k < 4; ++k) {
        VU1.micro_macflags[k] = state.mac;
        VU1.micro_clipflags[k] = state.clip;
        VU1.micro_statusflags[k] = state.status;
    }
    VU1.macflag = state.mac;
    VU1.clipflag = state.clip;
    VU1.statusflag = state.status;
    VU1.pending_q = state.q;
    VU1.pending_p = state.p;
    VU1.VI[REG_TPC].UL = state.pc >> 3;
    VU1.cycle = state.cycles;
}

void exportState(ps2x_microvu_state& state, uint32_t top, uint32_t itop,
                 uint64_t entry_cycle, uint32_t budget)
{
    for (unsigned r = 0; r < 32; ++r)
        for (unsigned lane = 0; lane < 4; ++lane)
            state.vf[r][lane] = VU1.VF[r].UL[lane];
    for (unsigned r = 0; r < 16; ++r)
        state.vi[r] = VU1.VI[r].UL;
    for (unsigned lane = 0; lane < 4; ++lane)
        state.acc[lane] = VU1.ACC.UL[lane];
    state.q = VU1.VI[REG_Q].UL;
    state.p = VU1.VI[REG_P].UL;
    state.i = VU1.VI[REG_I].UL;
    state.r = VU1.VI[REG_R].UL;
    state.pc = VU1.VI[REG_TPC].UL << 3;
    state.mac = VU1.VI[REG_MAC_FLAG].UL;
    state.clip = VU1.VI[REG_CLIP_FLAG].UL;
    state.status = VU1.VI[REG_STATUS_FLAG].UL;
    state.cycles = VU1.cycle;
    state.top = top;
    state.itop = itop;
    const uint32_t stat = VU0.VI[REG_VPU_STAT].UL;
    state.stopped_d = (stat & 0x200u) != 0u;
    state.stopped_t = (stat & 0x400u) != 0u;
    state.budget_exhausted = (VU1.cycle - entry_cycle >= budget) && (stat & 0x100u);
}
} // namespace

extern "C" PS2X_MV2_EXPORT uint32_t ps2x_microvu_abi() { return PS2X_MICROVU_ABI; }

extern "C" PS2X_MV2_EXPORT int ps2x_microvu_init(const char** error)
{
    if (error) *error = nullptr;
    if (s_ready) return 1;
    if (!recompiler_tests::RecompilerTestEnvironment::Initialize()) {
        if (error) *error = "ARMSX2 headless initialization failed";
        return 0;
    }
    auto fpcr = FPControlRegister::GetDefault().DisableExceptions()
        .SetDenormalsAreZero(true).SetFlushToZero(true);
    fpcr.SetRoundMode(FPRoundMode::ChopZero);
    EmuConfig.Cpu.VU1FPCR = fpcr;
    FPControlRegister::SetCurrent(fpcr);
    auto& rc = EmuConfig.Cpu.Recompiler;
    rc.vu1Overflow = true;
    rc.vu1ExtraOverflow = false;
    rc.vu1SignOverflow = false;
    rc.vu1ExactMode = false;
    EmuConfig.Speedhacks.vuThread = false;
    EmuConfig.Speedhacks.vu1Instant = false;
    EmuConfig.Speedhacks.vuFlagHack = false;
    EmuConfig.Gamefixes.XgKickHack = false;
    const char* lean = std::getenv("PS2X_MICROVU_BRIDGE_LEAN");
    s_leanDiff = !(lean && lean[0] == '0');
    s_ready = true;
    return 1;
}

extern "C" PS2X_MV2_EXPORT uint8_t* ps2x_microvu_vu1_data()
{
    return s_ready ? VU1.Mem : nullptr;
}

extern "C" PS2X_MV2_EXPORT void ps2x_microvu_shutdown()
{
    if (!s_ready) return;
    gif_test_hooks::g_path1_sink = nullptr;
    gif_test_hooks::g_path1_complete = nullptr;
    s_path1_fn = nullptr;
    s_path1_opaque = nullptr;
    s_path1.clear();
    recompiler_tests::RecompilerTestEnvironment::Shutdown();
    s_ready = false;
    s_seeded = false;
    s_code_generation = std::numeric_limits<uint64_t>::max();
}

extern "C" PS2X_MV2_EXPORT int ps2x_microvu_run(
    const uint8_t* code, uint32_t code_size, uint64_t generation,
    uint8_t* data, uint32_t data_size, uint32_t start_pc,
    uint32_t resume, uint32_t top, uint32_t itop, uint32_t fbrst,
    uint32_t budget, ps2x_microvu_state* state,
    ps2x_microvu_path1_fn path1, void* path1_opaque, const char** error)
{
    if (error) *error = nullptr;
    if (!s_ready || !code || !data || !state || !path1 ||
        code_size != 0x4000u || data_size != 0x4000u || !budget) {
        if (error) *error = "bad microVU run arguments or library not initialized";
        return 0;
    }
    if (!s_seeded) {
        seed(*state);
        s_seeded = true;
    }
    if (generation != s_code_generation) {
        uint32_t first = code_size;
        uint32_t last = 0;
        if (s_leanDiff) {
            first = firstDiff(VU1.Micro, code, code_size);
            if (first != code_size)
                last = lastDiffEnd(VU1.Micro, code, code_size, first);
        } else {
            for (uint32_t i = 0; i < code_size; ++i) {
                if (VU1.Micro[i] != code[i]) {
                    first = std::min(first, i);
                    last = i + 1;
                }
            }
        }
        std::memcpy(VU1.Micro, code, code_size);
        if (first == code_size) {
            first = 0;
            last = code_size;
        }
        first &= ~7u;
        last = (last + 7u) & ~7u;
        CpuMicroVU1.Clear(first, last - first);
        s_code_generation = generation;
    }
    // MP1 L1: a caller that keeps VU1 data in the library's memory
    // (ps2x_microvu_vu1_data) needs no staging copy either way.
    if (data != VU1.Mem)
        std::memcpy(VU1.Mem, data, data_size);
    vif1Regs.top = top;
    vif1Regs.itop = itop;
    VU0.VI[REG_FBRST].UL = fbrst;
    VU0.VI[REG_VPU_STAT].UL = (VU0.VI[REG_VPU_STAT].UL & ~0x700u) | 0x100u;
    VU1.VI[REG_VPU_STAT].UL = VU0.VI[REG_VPU_STAT].UL;
    if (!resume) {
        VU1.VI[REG_TPC].UL = (start_pc & 0x3ff8u) >> 3;
        CpuMicroVU1.SetStartPC(start_pc & 0x3ff8u);
    }
    const uint64_t entry_cycle = VU1.cycle;
    auto fpcr = EmuConfig.Cpu.VU1FPCR;
    FPControlRegister::SetCurrent(fpcr);
    s_path1_fn = path1;
    s_path1_opaque = path1_opaque;
    if (!resume)
        s_path1.clear();
    gif_test_hooks::g_path1_sink = &s_path1;
    gif_test_hooks::g_path1_complete = completePath1;
    gifUnit.gifPath[GIF_PATH_1].Reset();
    CpuMicroVU1.Execute(budget);
    gif_test_hooks::g_path1_sink = nullptr;
    gif_test_hooks::g_path1_complete = nullptr;
    s_path1_fn = nullptr;
    s_path1_opaque = nullptr;
    if (data != VU1.Mem)
        std::memcpy(data, VU1.Mem, data_size);
    exportState(*state, top, itop, entry_cycle, budget);
    return 1;
}
