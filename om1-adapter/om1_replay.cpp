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
#include "common/SHA256.h"
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

// ---- manifest validation (R3): typed accessors; malformed input is a clean
// exit-2 error, never an unchecked index or out-of-bounds memcpy. All range
// checks are subtraction-based (no mo+ln / so+ln signed overflow). ----
static constexpr long long kMicroSize = 16384;

[[noreturn]] static void mfail(const std::string& m)
{
    std::fprintf(stderr, "manifest: %s\n", m.c_str());
    std::exit(2);
}

static const JVal& reqKey(const JVal& v, const char* key, const char* ctx)
{
    if (v.t != JVal::T::Obj)
        mfail(std::string(ctx) + ": not an object");
    auto it = v.o.find(key);
    if (it == v.o.end())
        mfail(std::string(ctx) + ": missing '" + key + "'");
    return it->second;
}

static const JVal& reqArr(const JVal& v, const char* key, const char* ctx)
{
    const JVal& f = reqKey(v, key, ctx);
    if (f.t != JVal::T::Arr)
        mfail(std::string(ctx) + ": '" + key + "' is not an array");
    return f;
}

static long long reqNum(const JVal& v, const char* what, const char* ctx)
{
    if (v.t != JVal::T::Num)
        mfail(std::string(ctx) + ": '" + what + "' is not a number");
    return v.n;
}

static const std::string& reqStr(const JVal& v, const char* what, const char* ctx)
{
    if (v.t != JVal::T::Str)
        mfail(std::string(ctx) + ": '" + what + "' is not a string");
    return v.s;
}

static const JVal& reqIdx(const JVal& a, size_t i, const char* ctx)
{
    if (a.t != JVal::T::Arr || i >= a.a.size())
        mfail(std::string(ctx) + ": tuple index out of range");
    return a.a[i];
}

static void reqTupleLen(const JVal& a, size_t want, const char* ctx)
{
    if (a.t != JVal::T::Arr || a.a.size() != want)
        mfail(std::string(ctx) + ": tuple length mismatch");
}

// 0 <= start <= end <= kMicroSize, subtraction-based (no start+len overflow).
static void reqMicroRange(long long s, long long e, const char* ctx)
{
    if (s < 0 || e < s || e > kMicroSize)
        mfail(std::string(ctx) + ": range outside the 16 KiB Micro image");
}

static bool isHex(const std::string& h)
{
    for (char c : h)
    {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

// Full manifest walk before Attach/Initialize: every later exit(2) would run
// while backend threads are up. Checks types, tuple lengths, objIdx/startPC
// bounds and piece/range bounds (all subtraction-based).
static std::array<uint8_t, 16384> buildImage(const JVal& img, const std::vector<uint8_t>& elf);
static void validateManifest(const JVal& mad, const std::vector<uint8_t>& elf)
{
    const std::vector<JVal>& images = reqArr(mad, "images", "area").a;
    const std::vector<JVal>& blocks = reqArr(mad, "blocks", "area").a;
    for (const JVal& img : images)
    {
        if (img.t != JVal::T::Obj)
            mfail("image is not an object");
        for (const JVal& pc : reqArr(img, "pieces", "image").a)
        {
            reqTupleLen(pc, 3, "piece");
            long long mo = reqNum(reqIdx(pc, 0, "piece"), "mo", "piece");
            long long so = reqNum(reqIdx(pc, 1, "piece"), "so", "piece");
            long long ln = reqNum(reqIdx(pc, 2, "piece"), "ln", "piece");
            if (mo < 0 || ln < 0 || ln > kMicroSize - mo)
                mfail("piece outside the 16 KiB Micro image");
            if (so < 0 || ln > (long long)elf.size() - so)
                mfail("piece outside the ELF");
        }
        for (const JVal& r : reqArr(img, "ranges", "image").a)
        {
            reqTupleLen(r, 2, "range");
            reqMicroRange(reqNum(reqIdx(r, 0, "range"), "start", "range"),
                          reqNum(reqIdx(r, 1, "range"), "end", "range"), "range");
        }
    }
    for (const JVal& img : images)
    {
        // Image verification is pure (manifest + ELF bytes) so it runs here,
        // pre-Attach: a wrong-ELF invocation never spins up the backend.
        // (The same images are rebuilt below for the actual replay.)
        const std::string& want = reqStr(reqKey(img, "sha256", "image"), "sha256", "image");
        if (want.size() != 64 || !isHex(want))
            mfail("image sha256 is not 64 hex characters");
        std::array<uint8_t, 16384> probe = buildImage(img, elf);
        if (SHA256Digest::HashHex(probe.data(), probe.size()) != want)
            mfail("reconstructed image hash does not match the manifest (wrong ELF revision?)");
    }
    for (const JVal& b : blocks)
    {
        if (b.t != JVal::T::Obj)
            mfail("block is not an object");
        long long pc = reqNum(reqKey(b, "startPC", "block"), "startPC", "block");
        long long obj = reqNum(reqKey(b, "objIdx", "block"), "objIdx", "block");
        if (pc < 0 || pc >= kMicroSize || (pc & 7) != 0)
            mfail("block startPC outside the 16 KiB image or misaligned");
        if (obj < 0 || (size_t)obj >= images.size())
            mfail("block objIdx outside the area images");
        const std::string& ps = reqStr(reqKey(b, "pState_hex", "block"), "pState_hex", "block");
        if (ps.size() != 192 || !isHex(ps))
            mfail("block pState_hex is not 192 hex characters");
    }
}

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
    const long long elfSize = (long long)elf.size();
    for (const JVal& pc : reqArr(img, "pieces", "image").a)
    {
        reqTupleLen(pc, 3, "piece");
        long long mo = reqNum(reqIdx(pc, 0, "piece"), "mo", "piece");
        long long so = reqNum(reqIdx(pc, 1, "piece"), "so", "piece");
        long long ln = reqNum(reqIdx(pc, 2, "piece"), "ln", "piece");
        if (mo < 0 || ln < 0 || ln > kMicroSize - mo)
            mfail("piece outside the 16 KiB Micro image");
        if (so < 0 || ln > elfSize - so)
            mfail("piece outside the ELF");
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
    const std::vector<JVal>& rs = reqArr(img, "ranges", "image").a;
    std::vector<uint32_t> starts, ends;
    for (const JVal& r : rs)
    {
        reqTupleLen(r, 2, "range");
        long long s = reqNum(reqIdx(r, 0, "range"), "start", "range");
        long long e = reqNum(reqIdx(r, 1, "range"), "end", "range");
        reqMicroRange(s, e, "range");
        starts.push_back((uint32_t)s);
        ends.push_back((uint32_t)e);
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
    if (man.t != JVal::T::Obj)
        mfail("top level is not an object");
    const JVal& mad = reqKey(reqKey(man, "areas", "manifest"), area, "areas");
    const std::vector<JVal>& images = reqArr(mad, "images", "area").a;
    const std::vector<JVal>& blocks = reqArr(mad, "blocks", "area").a;

    // R8: the manifest pins the ELF revision; a same-sized wrong ELF would
    // otherwise compile unrelated bytes at valid offsets. Reject it here,
    // before Attach, by size and SHA-256 (image hashes are rechecked in
    // validateManifest below).
    const JVal& elfSec = reqKey(man, "elf", "manifest");
    const std::string& elfSha = reqStr(reqKey(elfSec, "sha256", "elf"), "sha256", "elf");
    long long elfWantSize = reqNum(reqKey(elfSec, "size", "elf"), "size", "elf");
    if (elfSha.size() != 64 || !isHex(elfSha) || elfWantSize < 0)
        mfail("elf pin is not a 64-hex sha256 with a non-negative size");
    std::ifstream ef(elfPath, std::ios::binary);
    if (!ef) { std::fprintf(stderr, "cannot open elf\n"); return 2; }
    std::vector<uint8_t> elf((std::istreambuf_iterator<char>(ef)), std::istreambuf_iterator<char>());
    if ((long long)elf.size() != elfWantSize)
        mfail("ELF size does not match the manifest pin");
    if (SHA256Digest::HashHex(elf.data(), elf.size()) != elfSha)
        mfail("ELF sha256 does not match the manifest pin (wrong disc revision?)");
    std::fprintf(stderr, "om1_replay: area=%s elf_bytes=%zu images=%zu blocks=%zu\n", area, elf.size(),
                 images.size(), blocks.size());
    validateManifest(mad, elf);

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
    for (const JVal& img : images)
        built.push_back(buildImage(img, elf));

    alignas(64) uint8_t pstate[96];
    long long cur = -1;
    for (size_t k = 0; k < blocks.size(); ++k)
    {
        const JVal& b = blocks[k];
        if (b.t != JVal::T::Obj)
            mfail("block is not an object");
        long long pc = reqNum(reqKey(b, "startPC", "block"), "startPC", "block");
        long long obj = reqNum(reqKey(b, "objIdx", "block"), "objIdx", "block");
        if (pc < 0 || pc >= kMicroSize || (pc & 7) != 0)
            mfail("block startPC outside the 16 KiB image or misaligned");
        if (obj < 0 || (size_t)obj >= images.size())
            mfail("block objIdx outside the area images");
        uint32_t startPC = (uint32_t)pc;
        hexToBytes(reqStr(reqKey(b, "pState_hex", "block"), "pState_hex", "block"), pstate, 96);
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
