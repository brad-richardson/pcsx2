// SPDX-FileCopyrightText: 2026 yaps2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// om1_replay: regenerate one OM1 stage area from the user's disc.
// Usage: om1_replay <manifest.json> <SLUS_207.72> <area> <outdir>
// A variant of om1-adapter/Record.cpp: instead of replaying a VU1CAP
// capture, it builds each 16 KB Micro image from frozen ELF pieces (zero
// fill outside the manifest ranges), seeds zeroed regs/Mem/VIF (Table G:
// codegen reads only Micro image + startPC + pState + config), injects the
// manifest ranges in place of mVUcacheProg-observed ones, dispatches every
// manifest triple through the resolve-only slow path (mVUexecuteVU1:
// lookup hit = no-op, miss = open/searchProg/close = one recorder
// episode), then om1::Write.
#include "RecompilerTestEnvironment.h"
#include "Config.h"
#include "VU.h"
#include "VUmicro.h"
#include "common/FPControl.h"
#include "om1/om1_hooks.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// ---- minimal JSON parser (objects, arrays, strings, numbers, literals) ----
struct JVal
{
    enum class T { Nul, Bool, Num, Str, Arr, Obj } t = T::Nul;
    bool b = false;
    long long n = 0;
    std::string s;
    std::vector<JVal> a;
    std::map<std::string, JVal> o;
};

struct JParse
{
    const char* p;
    [[noreturn]] void fail(const char* m) { std::fprintf(stderr, "json: %s\n", m); std::exit(2); }
    void ws() { while (*p && std::isspace((unsigned char)*p)) ++p; }
    JVal val()
    {
        ws();
        JVal v;
        if (*p == '{')
        {
            ++p; v.t = JVal::T::Obj;
            ws();
            if (*p == '}') { ++p; return v; }
            while (true)
            {
                ws();
                if (*p != '"') fail("obj key");
                std::string k = str();
                ws();
                if (*p != ':') fail("colon");
                ++p;
                v.o[k] = val();
                ws();
                if (*p == ',') { ++p; continue; }
                if (*p == '}') { ++p; break; }
                fail("obj sep");
            }
            return v;
        }
        if (*p == '[')
        {
            ++p; v.t = JVal::T::Arr;
            ws();
            if (*p == ']') { ++p; return v; }
            while (true)
            {
                v.a.push_back(val());
                ws();
                if (*p == ',') { ++p; continue; }
                if (*p == ']') { ++p; break; }
                fail("arr sep");
            }
            return v;
        }
        if (*p == '"') { v.t = JVal::T::Str; v.s = str(); return v; }
        if (!std::strncmp(p, "true", 4)) { p += 4; v.t = JVal::T::Bool; v.b = true; return v; }
        if (!std::strncmp(p, "false", 5)) { p += 5; v.t = JVal::T::Bool; v.b = false; return v; }
        if (!std::strncmp(p, "null", 4)) { p += 4; return v; }
        char* e = nullptr;
        v.n = std::strtoll(p, &e, 10);
        if (e == p) fail("number");
        p = e; v.t = JVal::T::Num;
        return v;
    }
    std::string str()
    {
        ++p; // opening quote
        std::string r;
        while (*p && *p != '"')
        {
            if (*p == '\\')
            {
                ++p;
                if (*p == 'n') r += '\n';
                else if (*p == 't') r += '\t';
                else if (*p) r += *p;
                if (*p) ++p;
            }
            else
                r += *p++;
        }
        if (*p != '"') fail("string end");
        ++p;
        return r;
    }
};

static unsigned hexNib(char c)
{
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (unsigned)(c - 'A' + 10);
    std::fprintf(stderr, "bad hex\n");
    std::exit(2);
}

static void hexToBytes(const std::string& h, uint8_t* out, size_t want)
{
    if (h.size() != want * 2) { std::fprintf(stderr, "hex len %zu want %zu\n", h.size(), want * 2); std::exit(2); }
    for (size_t i = 0; i < want; ++i)
        out[i] = (uint8_t)((hexNib(h[2 * i]) << 4) | hexNib(h[2 * i + 1]));
}

// Zeroed seed: mirror of Record.cpp seed() with all-capture values zero.
// Table G: codegen reads only Micro + startPC + pState + config, so zeros
// here are code-safe. Micro is loaded separately by the caller.
static void seedZeroed(uint32_t startPC)
{
    auto& vu = VU1;
    std::memset(vu.Mem, 0, 16384); // VU1 data memory is 16 KB (vu.Mem is a pointer)
    std::memset(vu.VF, 0, sizeof(vu.VF));
    std::memset(vu.VI, 0, sizeof(vu.VI));
    std::memset(vu.ACC.UL, 0, sizeof(vu.ACC.UL));
    vu.q.UL = vu.VI[REG_Q].UL = 0;
    vu.p.UL = vu.VI[REG_P].UL = 0;
    vu.VI[REG_I].UL = 0;
    vu.VI[REG_R].UL = 0;
    vu.VI[REG_MAC_FLAG].UL = 0;
    vu.VI[REG_CLIP_FLAG].UL = 0;
    vu.VI[REG_STATUS_FLAG].UL = 0;
    for (int k = 0; k < 4; ++k)
    {
        vu.micro_macflags[k] = 0;
        vu.micro_clipflags[k] = 0;
        vu.micro_statusflags[k] = 0;
    }
    vu.macflag = 0;
    vu.clipflag = 0;
    vu.statusflag = 0;
    vu.pending_q = 0;
    vu.pending_p = 0;
    vu.start_pc = startPC;
    vu.VI[REG_TPC].UL = startPC / 8;
    vu.cycle = 0;
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
    vif1Regs.top = 0;
    vif1Regs.itop = 0;
    vuRegs[0].VI[REG_VPU_STAT].UL =
        (vuRegs[0].VI[REG_VPU_STAT].UL & ~0xfffu) | 0x100u;
    vu.VI[REG_VPU_STAT].UL = vuRegs[0].VI[REG_VPU_STAT].UL;
    CpuMicroVU1.SetStartPC(startPC);
}

// Build one 16 KB Micro image from frozen ELF pieces; zero fill outside.
static std::array<uint8_t, 16384> buildImage(const JVal& img, const std::vector<uint8_t>& elf)
{
    std::array<uint8_t, 16384> out{};
    out.fill(0);
    for (const JVal& pc : img.o.at("pieces").a)
    {
        long long mo = pc.a[0].n, so = pc.a[1].n, ln = pc.a[2].n;
        if (mo < 0 || so < 0 || ln < 0 || mo + ln > 16384 || so + ln > (long long)elf.size())
        {
            std::fprintf(stderr, "piece out of range\n");
            std::exit(2);
        }
        std::memcpy(out.data() + mo, elf.data() + so, (size_t)ln);
    }
    return out;
}

// Backend-TU replay helpers (defined in pcsx2/arm64/microVU-arm64.cpp,
// which has the full microVU types; this TU must not include the backend
// headers). mVUexecuteVU1 is the resolve-only slow path: lookup hit
// compiles nothing, miss opens the cache, searchProgs and closes.
extern void* mVUexecuteVU1(uint32_t startPC, uint32_t cycles);
extern void om1ReplaySeedPState_VU1(const uint8_t pstate96[96]);
extern bool om1ReplaySyncProgRanges_VU1(const uint32_t* starts, const uint32_t* ends, size_t n);

// Inject the manifest ranges in place of mVUcacheProg-observed ones.
static bool injectRanges(const JVal& img)
{
    const std::vector<JVal>& rs = img.o.at("ranges").a;
    std::vector<uint32_t> starts, ends;
    for (const JVal& r : rs)
    {
        starts.push_back((uint32_t)r.a[0].n);
        ends.push_back((uint32_t)r.a[1].n);
    }
    return om1ReplaySyncProgRanges_VU1(starts.data(), ends.data(), starts.size());
}

int main(int argc, char** argv)
{
    // om1_replay <manifest.json> <SLUS_207.72> <area> <outdir>
    if (argc != 5)
    {
        std::fprintf(stderr, "usage: om1_replay <manifest> <elf> <area> <outdir>\n");
        return 2;
    }
    const char* manPath = argv[1];
    const char* elfPath = argv[2];
    const char* area = argv[3];
    const char* outdir = argv[4];

    std::ifstream mf(manPath);
    if (!mf) { std::fprintf(stderr, "cannot open manifest\n"); return 2; }
    std::stringstream mss;
    mss << mf.rdbuf();
    std::string mtext = mss.str();
    JParse jp{mtext.c_str()};
    JVal man = jp.val();
    const JVal& mad = man.o.at("areas").o.at(area);

    std::ifstream ef(elfPath, std::ios::binary);
    if (!ef) { std::fprintf(stderr, "cannot open elf\n"); return 2; }
    std::vector<uint8_t> elf((std::istreambuf_iterator<char>(ef)), std::istreambuf_iterator<char>());
    std::fprintf(stderr, "om1_replay: area=%s elf_bytes=%zu images=%zu blocks=%zu\n", area, elf.size(),
                 mad.o.at("images").a.size(), mad.o.at("blocks").a.size());

    // Attach before Initialize so Reserve-time emission is captured too.
    om1::Attach(1);
    if (!recompiler_tests::RecompilerTestEnvironment::Initialize())
    {
        std::fprintf(stderr, "PCSX2 headless initialization failed\n");
        return 3;
    }
    // Config pins from Record.cpp:226-237.
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

    // Pre-build every image from frozen ELF pieces (zero fill outside).
    std::vector<std::array<uint8_t, 16384>> built;
    for (const JVal& img : mad.o.at("images").a)
        built.push_back(buildImage(img, elf));

    const std::vector<JVal>& blocks = mad.o.at("blocks").a;
    const std::vector<JVal>& images = mad.o.at("images").a;
    alignas(64) uint8_t pstate[96];
    long long cur = -1;
    for (size_t k = 0; k < blocks.size(); ++k)
    {
        const JVal& b = blocks[k];
        uint32_t startPC = (uint32_t)b.o.at("startPC").n;
        long long obj = b.o.at("objIdx").n;
        hexToBytes(b.o.at("pState_hex").s, pstate, 96);
        if (obj != cur)
        {
            std::memcpy(VU1.Micro, built[(size_t)obj].data(), 16384);
            seedZeroed(startPC);
            CpuMicroVU1.Clear(0, 16384);
            cur = obj;
        }
        else
        {
            VU1.start_pc = startPC;
            VU1.VI[REG_TPC].UL = startPC / 8;
            CpuMicroVU1.SetStartPC(startPC);
        }
        om1ReplaySeedPState_VU1(pstate);
        // Resolve-only dispatch: lookup hit compiles nothing; miss opens
        // the cache, searchProgs (compile + recursive fan-out) and closes
        // (one recorder episode). Nothing is executed.
        void* e = mVUexecuteVU1(startPC, 65536);
        if (!e)
        {
            std::fprintf(stderr, "dispatch failed block=%zu pc=%04x\n", k, startPC);
            return 4;
        }
        if (!injectRanges(images[(size_t)obj]))
        {
            std::fprintf(stderr, "no prog block=%zu pc=%04x\n", k, startPC);
            return 4;
        }
    }

    int wrc = 0;
    om1::Detach();
    std::fprintf(stderr, "om1_replay: episodes=%u blocks=%u triples=%u resets=%u codeBytes=%llu\n",
                 om1::EpisodeCount(), om1::BlockCount(), om1::TripleCount(), om1::ResetCount(),
                 (unsigned long long)om1::CodeBytes());
    char tag[64];
    std::snprintf(tag, sizeof(tag), "%s", area);
    if (!om1::Write(outdir, tag))
    {
        std::fprintf(stderr, "om1_replay: write failed\n");
        wrc = 1;
    }
    recompiler_tests::RecompilerTestEnvironment::Shutdown();
    return wrc;
}
