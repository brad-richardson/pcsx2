// MV1 local feasibility adapter. ARMSX2 and ps2xRuntime are GPL-3 family.
#include "RecompilerTestEnvironment.h"
#include "Config.h"
#include "Gif_Unit.h"
#include "VU.h"
#include "VUmicro.h"
#include "common/FPControl.h"
#include "arm64/microVU_Persist-arm64.h"
#include "ps2_vu1cap.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static uint32_t bits(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }
static uint32_t load32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
static uint64_t ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static void seed(const ps2_vu1cap::Job& j, const ps2_vu1cap::Image& img,
                 const std::array<uint8_t, ps2_vu1cap::kDataSize>& data)
{
    auto& vu = VU1;
    std::memcpy(vu.Micro, img.code.data(), ps2_vu1cap::kCodeSize);
    std::memcpy(vu.Mem, data.data(), data.size());
    for (int r = 0; r < 32; ++r)
        for (int lane = 0; lane < 4; ++lane)
            vu.VF[r].UL[lane] = bits(j.regsIn.vf[r][lane]);
    for (int r = 0; r < 16; ++r)
        vu.VI[r].UL = static_cast<uint32_t>(j.regsIn.vi[r]);
    for (int lane = 0; lane < 4; ++lane)
        vu.ACC.UL[lane] = bits(j.regsIn.acc[lane]);
    vu.q.UL = vu.VI[REG_Q].UL = bits(j.regsIn.q);
    vu.p.UL = vu.VI[REG_P].UL = bits(j.regsIn.p);
    vu.VI[REG_I].UL = bits(j.regsIn.i);
    vu.VI[REG_R].UL = j.regsIn.r;
    vu.VI[REG_MAC_FLAG].UL = j.regsIn.mac;
    vu.VI[REG_CLIP_FLAG].UL = j.regsIn.clip;
    vu.VI[REG_STATUS_FLAG].UL = j.regsIn.status;
    for (int k = 0; k < 4; ++k) {
        vu.micro_macflags[k] = j.regsIn.mac;
        vu.micro_clipflags[k] = j.regsIn.clip;
        vu.micro_statusflags[k] = j.regsIn.status;
    }
    vu.macflag = j.regsIn.mac;
    vu.clipflag = j.regsIn.clip;
    vu.statusflag = j.regsIn.status;
    vu.pending_q = bits(j.regsIn.q);
    vu.pending_p = bits(j.regsIn.p);
    vu.start_pc = j.startPC;
    vu.VI[REG_TPC].UL = j.startPC / 8;
    vu.cycle = j.cycleStart;
    vu.ebit = vu.branch = vu.branchpc = vu.delaybranchpc = 0;
    vu.takedelaybranch = false;
    vu.flags = 0;
    vu.fmacreadpos = vu.fmacwritepos = vu.fmaccount = 0;
    vu.ialureadpos = vu.ialuwritepos = vu.ialucount = 0;
    std::memset(&vu.fdiv, 0, sizeof(vu.fdiv));
    std::memset(&vu.efu, 0, sizeof(vu.efu));
    std::memset(&vu.fmac, 0, sizeof(vu.fmac));
    std::memset(&vu.ialu, 0, sizeof(vu.ialu));
    vu.xgkickaddr = vu.xgkickdiff = vu.xgkicksizeremaining = 0;
    vu.xgkicklastcycle = 0;
    vu.xgkickcyclecount = vu.xgkickenable = vu.xgkickendpacket = 0;
    vif1Regs.top = j.top;
    vif1Regs.itop = j.itop;
    vuRegs[0].VI[REG_VPU_STAT].UL =
        (vuRegs[0].VI[REG_VPU_STAT].UL & ~0xfffu) | 0x100u;
    vu.VI[REG_VPU_STAT].UL = vuRegs[0].VI[REG_VPU_STAT].UL;
    CpuMicroVU1.SetStartPC(j.startPC);
}

struct FlagDiffStats {
    uint64_t mac = 0, clip = 0, status = 0;
};

struct ValueDiffStats {
    std::map<std::string, uint64_t> fields;
    std::map<uint32_t, uint64_t> images;
    uint64_t jobs = 0, values = 0, examples = 0;
    void add(uint64_t number, const ps2_vu1cap::Job& j, const char* field,
             unsigned index, uint64_t got, uint64_t want,
             std::map<std::string, uint64_t>& per_job) {
        ++fields[field]; ++images[j.imageIdx]; ++per_job[field]; ++values;
        if (examples++ < 10)
            std::fprintf(stderr,
              "VALUE_EXAMPLE job=%llu tick=%u image=%u entry=%04x field=%s[%u] got=%llx expected=%llx\n",
              (unsigned long long)number, j.tick, j.imageIdx, j.startPC, field, index,
              (unsigned long long)got, (unsigned long long)want);
    }
};

static void compare(uint64_t number, const ps2_vu1cap::Job& j,
                    const std::array<uint8_t, ps2_vu1cap::kDataSize>& expected,
                    const std::vector<uint8_t>& path,
                    const std::vector<u32>& packet_sizes,
                    FlagDiffStats& flag_diffs, ValueDiffStats& value_diffs)
{
    std::map<std::string, uint64_t> per_job;
    auto note = [&](const char* field, unsigned index, uint64_t got, uint64_t want) {
        value_diffs.add(number, j, field, index, got, want, per_job);
    };
    const auto* a = reinterpret_cast<const uint32_t*>(VU1.Mem);
    const auto* e = reinterpret_cast<const uint32_t*>(expected.data());
    for (unsigned i = 0; i < ps2_vu1cap::kDataWords; ++i)
        if (a[i] != e[i]) note("data_word", i, a[i], e[i]);
    for (unsigned r = 0; r < 32; ++r)
        for (unsigned lane = 0; lane < 4; ++lane)
            if (VU1.VF[r].UL[lane] != bits(j.regsOut.vf[r][lane]))
                note("vf_lane", r * 4 + lane, VU1.VF[r].UL[lane], bits(j.regsOut.vf[r][lane]));
    for (unsigned r = 0; r < 16; ++r)
        if ((VU1.VI[r].UL & 0xffff) != (static_cast<uint32_t>(j.regsOut.vi[r]) & 0xffff))
            note("vi", r, VU1.VI[r].UL, static_cast<uint32_t>(j.regsOut.vi[r]));
    for (unsigned lane = 0; lane < 4; ++lane)
        if (VU1.ACC.UL[lane] != bits(j.regsOut.acc[lane]))
            note("acc_lane", lane, VU1.ACC.UL[lane], bits(j.regsOut.acc[lane]));
    const struct { const char* name; uint32_t actual, expected; } fields[] = {
        {"q", VU1.VI[REG_Q].UL, bits(j.regsOut.q)},
        {"p", VU1.VI[REG_P].UL, bits(j.regsOut.p)},
        {"i", VU1.VI[REG_I].UL, bits(j.regsOut.i)},
        {"r", VU1.VI[REG_R].UL, j.regsOut.r},
        {"pc", VU1.VI[REG_TPC].UL * 8, j.regsOut.pc},
        {"top", vif1Regs.top, j.regsOut.top},
        {"itop", vif1Regs.itop, j.regsOut.itop},
    };
    for (const auto& f : fields)
        if (f.actual != f.expected) note(f.name, 0, f.actual, f.expected);
    const struct { const char* name; uint32_t actual, expected; uint64_t* count; } flags[] = {
        {"mac", VU1.VI[REG_MAC_FLAG].UL, j.regsOut.mac, &flag_diffs.mac},
        {"clip", VU1.VI[REG_CLIP_FLAG].UL, j.regsOut.clip, &flag_diffs.clip},
        {"status", VU1.VI[REG_STATUS_FLAG].UL, j.regsOut.status, &flag_diffs.status},
    };
    for (const auto& f : flags)
        if (f.actual != f.expected) {
            if ((*f.count)++ == 0)
                std::fprintf(stderr,
                  "FLAG_AMBIGUOUS job=%llu tick=%u entry=%04x field=%s got=%08x expected=%08x (capture has no four-deep flag pipeline)\n",
                  (unsigned long long)number, j.tick, j.startPC, f.name, f.actual, f.expected);
        }
    if (packet_sizes.size() != j.xgkSize.size())
        note("packet_count", 0, packet_sizes.size(), j.xgkSize.size());
    size_t actual_offset = 0, expected_offset = 0;
    for (size_t p = 0; p < std::min(packet_sizes.size(), j.xgkSize.size()); ++p) {
        if (packet_sizes[p] != j.xgkSize[p])
            note("packet_size", static_cast<unsigned>(p), packet_sizes[p], j.xgkSize[p]);
        if (actual_offset + packet_sizes[p] > path.size() ||
            expected_offset + j.xgkSize[p] > j.xgkBytes.size()) {
            note("packet_truncated", static_cast<unsigned>(p), path.size(), j.xgkBytes.size());
            break;
        }
        for (size_t b = 0; b < std::min(packet_sizes[p], j.xgkSize[p]); ++b)
            if (path[actual_offset + b] != j.xgkBytes[expected_offset + b]) {
                note("packet_byte", static_cast<unsigned>(p), path[actual_offset+b], j.xgkBytes[expected_offset+b]);
            }
        actual_offset += packet_sizes[p];
        expected_offset += j.xgkSize[p];
    }
    if (actual_offset != path.size() || expected_offset != j.xgkBytes.size())
        note("packet_total", 0, path.size(), j.xgkBytes.size());
    if (!per_job.empty()) {
        ++value_diffs.jobs;
        std::fprintf(stderr, "VALUE_JOB job=%llu tick=%u image=%u entry=%04x count=",
          (unsigned long long)number, j.tick, j.imageIdx, j.startPC);
        uint64_t n = 0;
        for (const auto& [field, count] : per_job) {
            std::fprintf(stderr, "%s%s:%llu", n++ ? "," : "", field.c_str(),
                         (unsigned long long)count);
        }
        std::fprintf(stderr, " output_pc=%04x cycle_delta=%lld\n", VU1.VI[REG_TPC].UL * 8,
          (long long)(int64_t(VU1.cycle) - int64_t(j.cycleEnd)));
    }
}

struct CycleStats {
    std::vector<int64_t> values;
    int64_t sum = 0;
    void add(int64_t v) { values.push_back(v); sum += v; }
};

static void printStats(const char* kind, unsigned long long key, CycleStats& s)
{
    std::sort(s.values.begin(), s.values.end());
    const auto n = s.values.size();
    if (!n) return;
    const auto p50 = s.values[(n - 1) / 2];
    const auto p95 = s.values[((n - 1) * 95) / 100];
    size_t neg = 0, zero = 0, pos = 0;
    for (int64_t v : s.values) { if (v < 0) ++neg; else if (v > 0) ++pos; else ++zero; }
    std::printf("cycle_%s key=%llu jobs=%zu sum=%lld mean=%.3f min=%lld p50=%lld p95=%lld max=%lld neg=%zu zero=%zu pos=%zu\n",
      kind, key, n, (long long)s.sum, double(s.sum) / n,
      (long long)s.values.front(), (long long)p50, (long long)p95,
      (long long)s.values.back(), neg, zero, pos);
}

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 4) { std::fprintf(stderr, "usage: mv1_replay <VU1CAP01> [passes] [speed]\n"); return 2; }
    const int passes = argc >= 3 ? std::atoi(argv[2]) : 1;
    const bool speed = argc == 4 && std::strcmp(argv[3], "speed") == 0;
    if (argc == 4 && !speed) { std::fprintf(stderr, "unknown mode: %s\n", argv[3]); return 2; }
    if (passes < 1 || passes > 8) { std::fprintf(stderr, "passes must be 1..8\n"); return 2; }
    ps2_vu1cap::Reader reader;
    std::string err;
    if (!reader.open(argv[1], err)) { std::fprintf(stderr, "capture: %s\n", err.c_str()); return 2; }
    if (!recompiler_tests::RecompilerTestEnvironment::Initialize()) {
        std::fprintf(stderr, "PCSX2 headless initialization failed\n"); return 3;
    }
    auto fpcr = FPControlRegister::GetDefault().DisableExceptions()
        .SetDenormalsAreZero(true).SetFlushToZero(true);
    fpcr.SetRoundMode(FPRoundMode::ChopZero);
    EmuConfig.Cpu.VU1FPCR = fpcr;
    FPControlRegister::SetCurrent(fpcr);
    auto& rc = EmuConfig.Cpu.Recompiler;
    rc.vu1Overflow = true; rc.vu1ExtraOverflow = false;
    rc.vu1SignOverflow = false; rc.vu1ExactMode = false;
    EmuConfig.Speedhacks.vuThread = false;
    EmuConfig.Speedhacks.vu1Instant = false;
    EmuConfig.Speedhacks.vuFlagHack = false;
    EmuConfig.Gamefixes.XgKickHack = false;
    std::fprintf(stderr, "settings: Normal clamp=1,0,0,0 FPCR=DAZ+FZ round=zero flags=full XgKickHack=off vu1Instant=off cycle_budget=65536 passes=%d mode=%s\n", passes, speed ? "speed" : "report");
    std::array<uint8_t, ps2_vu1cap::kDataSize> data{}, expected{}, prev{};
    std::vector<uint8_t> path;
    std::vector<u32> packet_sizes;
    gif_test_hooks::g_path1_sink = speed ? nullptr : &path;
    gif_test_hooks::g_path1_packet_sizes = speed ? nullptr : &packet_sizes;
    gif_test_hooks::g_path1_discard = speed;
    auto cleanup = [] {
        gif_test_hooks::g_path1_sink = nullptr;
        gif_test_hooks::g_path1_packet_sizes = nullptr;
        gif_test_hooks::g_path1_discard = false;
        recompiler_tests::RecompilerTestEnvironment::Shutdown();
    };
    ps2_vu1cap::Job job;
    CycleStats all_cycles;
    std::map<uint64_t, CycleStats> image_cycles;
    std::map<uint32_t, CycleStats> tick_cycles;
    FlagDiffStats flag_diffs;
    ValueDiffStats value_diffs;
    std::vector<uint64_t> pass_ns, first_job_ns;
    uint64_t total_jobs = 0;
    const uint64_t tick_span = reader.tickTo() - reader.tickFrom() + 1;
    for (size_t i = 0; i < reader.images().size(); ++i)
        std::printf("image_index idx=%zu key=%016llx\n", i,
          (unsigned long long)reader.images()[i].key);
    for (int pass = 0; pass < passes; ++pass) {
        reader.rewindJobs();
        prev.fill(0);
        int current_image = -1;
        uint64_t pass_jobs = 0, execution_ns = 0, first_ns = 0;
        err.clear();
        while (reader.readJob(job, err)) {
            if (job.imageIdx >= reader.images().size()) { err = "bad image index"; break; }
            if (job.inFull) std::memcpy(data.data(), job.inFullBytes.data(), data.size());
            else {
                std::memcpy(data.data(), prev.data(), data.size());
                for (size_t k = 0; k < job.inIdx.size(); ++k)
                    std::memcpy(data.data() + job.inIdx[k] * 4, &job.inVal[k], 4);
            }
            expected = data;
            for (size_t k = 0; k < job.outIdx.size(); ++k)
                std::memcpy(expected.data() + job.outIdx[k] * 4, &job.outVal[k], 4);
            seed(job, reader.images()[job.imageIdx], data);
            if (static_cast<int>(job.imageIdx) != current_image) {
                CpuMicroVU1.Clear(0, ps2_vu1cap::kCodeSize);
                current_image = static_cast<int>(job.imageIdx);
            }
            path.clear(); packet_sizes.clear();
            gifUnit.gifPath[GIF_PATH_1].Reset();
            const bool trace846 = !speed && pass == 0 && pass_jobs == 846 &&
                job.startPC == 0x3a08 && reader.images()[job.imageIdx].key == 0xf587398bb6fc4651ull;
            if (trace846)
                std::fprintf(stderr,
                  "TRACE846 microVU entry vf1x=%08x vf13x=%08x vf17x=%08x accx=%08x vf12x=%08x q=%08x code1480=%08x/%08x code14a8=%08x/%08x\n",
                  VU1.VF[1].UL[0], VU1.VF[13].UL[0], VU1.VF[17].UL[0], VU1.ACC.UL[0],
                  VU1.VF[12].UL[0], VU1.VI[REG_Q].UL,
                  load32(reader.images()[job.imageIdx].code.data()+0x1480),
                  load32(reader.images()[job.imageIdx].code.data()+0x1484),
                  load32(reader.images()[job.imageIdx].code.data()+0x14a8),
                  load32(reader.images()[job.imageIdx].code.data()+0x14ac));
            const auto t0 = ns();
            CpuMicroVU1.Execute(65536);
            const auto elapsed = ns() - t0;
            execution_ns += elapsed;
            if (pass_jobs == 0) first_ns = elapsed;
            const int64_t cycle_delta = int64_t(VU1.cycle) - int64_t(job.cycleEnd);
            if (trace846)
                std::fprintf(stderr,
                  "TRACE846 microVU exit vf1x=%08x vf13x=%08x vf17x=%08x accx=%08x vf12x=%08x q=%08x\n",
                  VU1.VF[1].UL[0], VU1.VF[13].UL[0], VU1.VF[17].UL[0], VU1.ACC.UL[0],
                  VU1.VF[12].UL[0], VU1.VI[REG_Q].UL);
            if (pass == 0 && !speed) {
                all_cycles.add(cycle_delta);
                image_cycles[reader.images()[job.imageIdx].key].add(cycle_delta);
                tick_cycles[job.tick].add(cycle_delta);
            }
            if (pass == 0 && !speed) {
                compare(pass_jobs, job, expected, path, packet_sizes, flag_diffs, value_diffs);
                if (value_diffs.jobs * 100 > reader.jobCount()) {
                    std::fprintf(stderr, "STOP_WIDESPREAD value_jobs=%llu capture_jobs=%llu threshold=1_percent\n",
                      (unsigned long long)value_diffs.jobs, (unsigned long long)reader.jobCount());
                    cleanup();
                    return 4;
                }
            }
            prev = expected;
            ++pass_jobs;
        }
        if (!err.empty()) { std::fprintf(stderr, "reader: %s\n", err.c_str()); cleanup(); return 2; }
        total_jobs += pass_jobs;
        pass_ns.push_back(execution_ns);
        first_job_ns.push_back(first_ns);
        std::printf("pass=%d jobs=%llu ticks=%llu execution_ms=%.3f ms_per_tick=%.6f first_job_ms=%.6f\n",
          pass, (unsigned long long)pass_jobs, (unsigned long long)tick_span,
          execution_ns / 1e6, double(execution_ns) / 1e6 / tick_span, first_ns / 1e6);
        std::fflush(stdout);
    }
    if (!speed) {
        printStats("all", 0, all_cycles);
        for (auto& [image, stats] : image_cycles) printStats("image", image, stats);
        for (auto& [tick, stats] : tick_cycles) printStats("tick", tick, stats);
        for (const auto& [field, count] : value_diffs.fields)
            std::printf("value_field field=%s count=%llu\n", field.c_str(), (unsigned long long)count);
        for (const auto& [image, count] : value_diffs.images)
            std::printf("value_image idx=%u count=%llu\n", image, (unsigned long long)count);
        std::printf("value_summary jobs=%llu of=%llu percent=%.6f fields=%llu examples=%llu\n",
          (unsigned long long)value_diffs.jobs, (unsigned long long)reader.jobCount(),
          100.0 * double(value_diffs.jobs) / double(reader.jobCount()),
          (unsigned long long)value_diffs.values,
          (unsigned long long)std::min<uint64_t>(value_diffs.examples, 10));
        std::printf("flag_ambiguous mac=%llu clip=%llu status=%llu\n",
          (unsigned long long)flag_diffs.mac, (unsigned long long)flag_diffs.clip,
          (unsigned long long)flag_diffs.status);
    }
    if (passes > 1) {
        std::vector<uint64_t> warm(pass_ns.begin() + 1, pass_ns.end());
        std::sort(warm.begin(), warm.end());
        const uint64_t warm_median = warm[(warm.size()-1)/2];
        std::printf("cold_pass_ms=%.3f warm_pass_median_ms=%.3f cold_extra_estimate_ms=%.3f cold_first_job_ms=%.6f warm_first_job_median_ms=%.6f\n",
          pass_ns[0] / 1e6, warm_median / 1e6,
          (double(pass_ns[0])-double(warm_median)) / 1e6,
          first_job_ns[0] / 1e6, first_job_ns[1] / 1e6);
    }
    std::printf("checked=%llu passes=%d value_mismatch_jobs=%llu mode=%s\n",
      (unsigned long long)total_jobs, passes, (unsigned long long)value_diffs.jobs,
      speed ? "speed" : "report");
    {
        const auto st = mVUPersist::GetStats(1);
        std::printf("mvu_blocks vu1_compiles=%llu chunks=%llu dropped=%llu blocks=%llu fixups=%llu hydrated_progs=%llu hydrated_blocks=%llu rejects=%llu\n",
          (unsigned long long)mVUPersist::GetBlockCompileCount(1),
          (unsigned long long)st.chunksRecorded, (unsigned long long)st.chunksDropped,
          (unsigned long long)st.blocksRecorded, (unsigned long long)st.fixupsRecorded,
          (unsigned long long)st.programsHydrated, (unsigned long long)st.blocksHydrated,
          (unsigned long long)st.hydrationRejects);
    }
    cleanup();
    return 0;
}
