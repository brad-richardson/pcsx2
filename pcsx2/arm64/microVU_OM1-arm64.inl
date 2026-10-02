// OM1 route-a capture recorder + .s writer. OM1-private.
// Single-TU inclusion (same pattern as microVU_Persist-arm64.inl): needs the
// TU-local mVU* functions and the microVU/microBlock/microProgram types.
// Attached during a JIT replay; records every address-bearing emission with
// chunk-relative offsets, then writes a layout-preserving .s (all intra-chunk
// PC-relative code stays byte-valid) plus consumer tables.
#include "../../om1/om1_hooks.h"

#include "arm64/AsmHelpers.h"
#include "arm64/microVU-arm64.h"

#include <cctype>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace om1
{
bool om1_no_codegen_flag = false; // strong def for this tool; consumer has its own
namespace
{

enum FixKind : u8
{
    FK_AbsMov = 0, // canonical movz+movk x3 (16B): reg + absolute target
    FK_AbsCall,    // canonical 16B mov + blr/br x16: same + call flag
    FK_Branch26,   // B/BL imm26 (4B): code/symbol target + isCall
    FK_Adrp,       // ADRP (4B) + Add/Orr (4B): absolute target (expect none)
};

struct Fixup
{
    u32 off = 0;
    FixKind kind = FK_AbsMov;
    u8 reg = 0; // dest reg for mov kinds
    bool isCall = false;
    u64 target = 0;
    // SelfBlockAbs resolution (filled at record time; valid iff heapBlock):
    bool heapBlock = false;
    u32 blockIdx = 0; // index into global blocks[]
    u32 fieldOff = 0;
};

struct BlockRec
{
    u32 id = 0;
    u32 gen = 0;
    u32 chunk = 0; // episode id (filled at End)
    u64 entryAddr = 0;
    u32 entryOff = 0; // filled at End
    u32 startPC = 0;
    u8 pState[96] = {};
    u8 pStateEnd[96] = {};
    bool pStateEndValid = false;
    bool hasJC = false;
    u64 hashLo = 0, hashHi = 0;
    microBlock* live = nullptr; // valid only until episode End
};

struct StubSnap
{
    std::string name;
    u64 addr = 0;
};

struct Episode
{
    u32 id = 0;
    u32 gen = 0;
    int vu = 1;
    u64 base = 0;
    std::vector<u8> bytes;
    std::vector<Fixup> fixups;
    std::vector<u32> blockIds; // into g_blocks
    std::vector<StubSnap> stubs;
};

struct ProgRec
{
    u64 lo = 0, hi = 0;
    std::vector<u8> data; // 16KB cached image
    std::vector<std::pair<u16, u16>> ranges;
};

microVU* vuPtr(int idx) { return idx ? &microVU1 : &microVU0; }

bool g_debug = false;
#define OM1_DBG(...) do { if (g_debug) std::fprintf(stderr, "[om1-dbg] " __VA_ARGS__); } while (0)

// ---- global capture state ----
bool g_attached = false;
int g_vu = 1;
u32 g_gen = 0;
u32 g_resets = 0;
bool g_episodeOpen = false;
Episode g_cur;
std::vector<Episode> g_episodes;
std::vector<BlockRec> g_blocks;
std::map<std::pair<u64, u64>, ProgRec> g_progs; // by (lo,hi)
std::map<const void*, std::string> g_known;     // exact-addr overrides
u64 g_errors = 0;

void noteError(const char* fmt, ...)
{
    ++g_errors;
    std::fprintf(stderr, "[om1] ERROR: ");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
}

bool inSlab(microVU* m, const void* a)
{
    const u8* p = static_cast<const u8*>(a);
    return p >= m->cache && p < m->prog.x86end + (mVUcacheSafeZone * _1mb);
}

class Recorder final : public ArmAddressRecorder
{
public:
    microVU* mvu = nullptr;
    u8* chunkBase = nullptr;

    u32 offOf(u8* at) const { return static_cast<u32>(at - chunkBase); }
    bool inChunk(const void* a) const
    {
        const u8* p = static_cast<const u8*>(a);
        return p >= chunkBase && p < armGetCurrentCodePointer();
    }

    MoveForm ClassifyMove(const void*) override { return MoveForm::CanonicalAbs; }
    bool WantsLongCondBranch(const void* t) override
    {
        bool inC = inChunk(t), inS = inSlab(mvu, t);
        OM1_DBG("cond t=%p base=%p cur=%p inChunk=%d cache=%p x86end=%p inSlab=%d -> %d\n", t,
                chunkBase, armGetCurrentCodePointer(), (int)inC, (void*)mvu->cache,
                (void*)mvu->prog.x86end, (int)inS, (int)(!inC && inS));
        return !inC && inS; // mirror persist (safe)
    }
    void OnCanonicalAbsMove(u8* at, const void* addr) override
    {
        Fixup f;
        f.off = offOf(at);
        f.kind = FK_AbsMov;
        f.target = reinterpret_cast<u64>(addr);
        // SelfBlockAbs resolution at record time (heap ptrs die at reset).
        const u8* a = static_cast<const u8*>(addr);
        for (u32 id : g_cur.blockIds)
        {
            BlockRec& b = g_blocks[id];
            const u8* blk = reinterpret_cast<const u8*>(b.live);
            if (b.live && a >= blk && a < blk + sizeof(microBlock))
            {
                f.heapBlock = true;
                f.blockIdx = id;
                f.fieldOff = static_cast<u32>(a - blk);
                break;
            }
        }
        g_cur.fixups.push_back(f);
    }
    void OnDirectBranch(u8* at, const void* t, bool isCall) override
    {
        OM1_DBG("branch at=%p t=%p call=%d inChunk=%d\n", at, t, (int)isCall,
                (int)inChunk(t));
        if (inChunk(t))
            return; // intra-chunk: layout-preserved, no record needed
        Fixup f;
        f.off = offOf(at);
        f.kind = FK_Branch26;
        f.isCall = isCall;
        f.target = reinterpret_cast<u64>(t);
        g_cur.fixups.push_back(f);
    }
    void OnAdrp(u8* at, const void* addr) override
    {
        Fixup f;
        f.off = offOf(at);
        f.kind = FK_Adrp;
        f.target = reinterpret_cast<u64>(addr);
        g_cur.fixups.push_back(f);
    }
    void OnAbsoluteTarget(const void* t) override
    {
        // Should be unreachable while recording: moves are canonical-forced
        // and call/jmp out-of-range paths report OnAbsoluteCallSite.
        noteError("OnAbsoluteTarget escaped: %p", t);
    }
    void OnAbsoluteCallSite(u8* at, const void* t, bool isCall) override
    {
        Fixup f;
        f.off = offOf(at);
        f.kind = FK_AbsCall;
        f.isCall = isCall;
        f.reg = 16;
        f.target = reinterpret_cast<u64>(t);
        g_cur.fixups.push_back(f);
    }
};

Recorder g_rec;

} // namespace

bool Active() { return g_attached; }
void Attach(int vuIndex)
{
    g_debug = (std::getenv("OM1_DEBUG") != nullptr);
    g_vu = vuIndex;
    g_rec.mvu = vuPtr(vuIndex);
    armAddressRecorder = &g_rec;
    g_attached = true;
}
void Detach()
{
    if (armAddressRecorder == &g_rec)
        armAddressRecorder = nullptr;
    g_attached = false;
}

void EpisodeBegin(int vuIndex, u8* base)
{
    if (!g_attached || vuIndex != g_vu)
        return;
    if (g_episodeOpen)
    {
        noteError("nested EpisodeBegin");
        return;
    }
    g_cur = Episode();
    g_cur.id = static_cast<u32>(g_episodes.size());
    g_cur.gen = g_gen;
    g_cur.vu = vuIndex;
    g_cur.base = reinterpret_cast<u64>(base);
    g_rec.mvu = vuPtr(vuIndex);
    g_rec.chunkBase = base;
    armAddressRecorder = &g_rec;
    g_episodeOpen = true;
    OM1_DBG("begin id=%u gen=%u base=%p\n", g_cur.id, g_cur.gen, base);
}

void EpisodeEnd(int vuIndex, u8* end)
{
    if (!g_attached || vuIndex != g_vu)
        return;
    if (!g_episodeOpen)
    {
        noteError("EpisodeEnd with none open");
        return;
    }
    u8* base = reinterpret_cast<u8*>(g_cur.base);
    if (end < base)
    {
        noteError("negative episode size");
        g_episodeOpen = false;
        return;
    }
    g_cur.bytes.assign(base, end);
    OM1_DBG("end id=%u size=%u fixups=%u blocks=%u\n", g_cur.id,
            (unsigned)g_cur.bytes.size(), (unsigned)g_cur.fixups.size(),
            (unsigned)g_cur.blockIds.size());
    if (g_cur.bytes.empty() && g_cur.blockIds.empty() && g_cur.fixups.empty())
    {
        // Lookup hit, no emission: drop the empty episode (keeps ids dense).
        g_cur = Episode();
        g_episodeOpen = false;
        return;
    }
    microVU* m = vuPtr(vuIndex);
    // Finalize blocks: offsets, pStateEnd, jumpCache presence.
    for (u32 id : g_cur.blockIds)
    {
        BlockRec& b = g_blocks[id];
        if (b.entryAddr < g_cur.base || b.entryAddr >= reinterpret_cast<u64>(end))
        {
            noteError("block %u entry %llx outside episode %u [%llx,%llx)", id,
                      (unsigned long long)b.entryAddr, g_cur.id,
                      (unsigned long long)g_cur.base, (unsigned long long)end);
            continue;
        }
        b.entryOff = static_cast<u32>(b.entryAddr - g_cur.base);
        b.chunk = g_cur.id;
        if (b.live)
        {
            std::memcpy(b.pStateEnd, &b.live->pStateEnd, 96);
            b.pStateEndValid = true;
            b.hasJC = (b.live->jumpCache != nullptr);
            b.live = nullptr;
        }
    }
    // Snapshot stub addresses (fields just emitted / still live).
    auto snap = [&](const char* n, u8* p) {
        if (p)
            g_cur.stubs.push_back({n, reinterpret_cast<u64>(p)});
    };
    snap("startFunct", m->startFunct);
    snap("startFunctResume", m->startFunctResume);
    snap("exitFunct", m->exitFunct);
    snap("exitFunctEBit", m->exitFunctEBit);
    snap("startFunctXG", m->startFunctXG);
    snap("exitFunctXG", m->exitFunctXG);
    snap("waitMTVU", m->waitMTVU);
    snap("copyPLState", m->copyPLState);
    snap("endProgramFlagsA", m->endProgramFlagsA);
    snap("endProgramFlagsB", m->endProgramFlagsB);
    snap("resumePtrXG", m->resumePtrXG);
    snap("cycleBreak", m->cycleBreak);
    for (int i = 0; i < mVUModelStubCount; ++i)
    {
        if (m->modelStubs[i])
        {
            char n[32];
            std::snprintf(n, sizeof(n), "model%d", i);
            snap(strdup(n), m->modelStubs[i]);
        }
    }
    g_episodes.push_back(std::move(g_cur));
    g_cur = Episode();
    g_episodeOpen = false;
}

void BlockCompiled(int vuIndex, microBlock* blk, u8* entry, u32 startPC)
{
    if (!g_attached || vuIndex != g_vu || !blk)
        return;
    if (!g_episodeOpen)
    {
        noteError("BlockCompiled with no open episode (pc=%x)", startPC);
        return;
    }
    BlockRec b;
    b.id = static_cast<u32>(g_blocks.size());
    b.gen = g_gen;
    b.entryAddr = reinterpret_cast<u64>(entry);
    b.startPC = startPC;
    std::memcpy(b.pState, &blk->pState, 96);
    microVU* m = vuPtr(vuIndex);
    if (m->prog.cur)
    {
        b.hashLo = m->prog.cur->contentHash.low64;
        b.hashHi = m->prog.cur->contentHash.high64;
        // Snapshot the program. Both data and ranges are last-wins: the
        // hook fires at first-pass init, when the current range is still
        // open (end=-1) and mVUcacheProg has not yet synced the block's
        // bytes into prog.data — so first-wins catches open ranges and
        // zero/partial data. Write() refreshes both once more from the
        // live programs at end of run, when every compile has finished.
        auto key = std::make_pair(b.hashLo, b.hashHi);
        auto pit = g_progs.find(key);
        if (pit == g_progs.end())
        {
            ProgRec p;
            p.lo = b.hashLo;
            p.hi = b.hashHi;
            p.data.assign(reinterpret_cast<u8*>(m->prog.cur->data),
                          reinterpret_cast<u8*>(m->prog.cur->data) + sizeof(m->prog.cur->data));
            if (m->prog.cur->ranges)
                for (const microRange& r : *m->prog.cur->ranges)
                    p.ranges.emplace_back(static_cast<u16>(r.start), static_cast<u16>(r.end));
            g_progs[key] = std::move(p);
        }
        else
        {
            // Data is last-wins too: mVUcacheProg fills prog.data
            // incrementally (range-by-range as compilation proceeds), so the
            // first-wins snapshot is zeros. Refresh both snapshot and ranges.
            pit->second.data.assign(
                reinterpret_cast<u8*>(m->prog.cur->data),
                reinterpret_cast<u8*>(m->prog.cur->data) + sizeof(m->prog.cur->data));
            if (m->prog.cur->ranges)
            {
                pit->second.ranges.clear();
                for (const microRange& r : *m->prog.cur->ranges)
                    pit->second.ranges.emplace_back(static_cast<u16>(r.start),
                                                    static_cast<u16>(r.end));
            }
        }
    }
    b.live = blk;
    g_cur.blockIds.push_back(b.id);
    g_blocks.push_back(b);
}

void OnReset(int vuIndex)
{
    if (!g_attached || vuIndex != g_vu)
        return;
    ++g_gen;
    ++g_resets;
}

void AddKnownSymbol(const void* addr, const char* name) { g_known[addr] = name; }
u32 EpisodeCount() { return static_cast<u32>(g_episodes.size()); }
u32 BlockCount() { return static_cast<u32>(g_blocks.size()); }
u32 ResetCount() { return g_resets; }
unsigned long long CodeBytes()
{
    unsigned long long n = 0;
    for (const auto& e : g_episodes)
        n += e.bytes.size();
    return n;
}
u32 TripleCount()
{
    std::set<std::string> s;
    for (const auto& b : g_blocks)
    {
        std::string k;
        k.append(reinterpret_cast<const char*>(&b.hashLo), 8);
        k.append(reinterpret_cast<const char*>(&b.hashHi), 8);
        k.append(reinterpret_cast<const char*>(&b.startPC), 4);
        k.append(reinterpret_cast<const char*>(b.pState), 96);
        s.insert(k);
    }
    return static_cast<u32>(s.size());
}

namespace
{

u32 wordAt(const Episode& e, u32 off)
{
    u32 w = 0;
    std::memcpy(&w, e.bytes.data() + off, 4);
    return w;
}

s64 signExtend(u64 v, int bits) { return (s64)(v << (64 - bits)) >> (64 - bits); }

// ---- symbol resolution ----
// Returns the .s operand for absolute target T (a label, a symbol+addend, or
// an init-slot reference). Sets *initSlot when the target needs a consumer
// init-filled pointer (heap microBlock fields).
struct SymRef
{
    std::string text; // .s operand, e.g. "_foo+0x10" or "Lom1q_3"
    bool isInitSlot = false;
    u32 blockIdx = 0;
    u32 fieldOff = 0;
};

// (gen,addr) -> code label, for branch targets inside recorded chunks.
std::map<std::pair<u32, u64>, std::string> g_codeLabels;

// dladdr on macOS strips one leading underscore: C `vuState` arrives bare,
// C++ `__Z...` (double, as in the ARMSX2 objects) arrives as `_Z...`. Restore
// it so the .s references the linked name — except the three TU-local statics
// the consumer re-provides under exact asm labels (their ARMSX2 originals
// have no external linkage, so the stripped form is the final form). Any
// other _ZL ref is a loud error: it needs a new consumer replica.
std::string machoSym(const char* s)
{
    if (!std::strcmp(s, "_ZL7mVUglob") || !std::strcmp(s, "_ZL7mVUEBitv") ||
        !std::strcmp(s, "_ZL11mVUwaitMTVUv"))
        return s;
    if (s[0] == '_' && s[1] == 'Z' && s[2] == 'L')
    {
        noteError("unprovided TU-local static %s (needs a consumer replica)", s);
        return std::string("_") + s; // unreachable: errors abort the write
    }
    return std::string("_") + s;
}

SymRef resolveData(u64 t)
{
    // Exact-addr overrides (used for the mVUcompileJIT special-case).
    auto it = g_known.find(reinterpret_cast<const void*>(t));
    if (it != g_known.end())
        return {it->second, false, 0, 0};
    Dl_info info;
    if (dladdr(reinterpret_cast<const void*>(t), &info) && info.dli_sname)
    {
        // Offline jump misses resolve through our own resolver, never the JIT.
        // (Matched by name: taking its address would instantiate the template
        // in this TU, which can't link under hidden visibility.)
        if (std::strstr(info.dli_sname, "mVUcompileJIT"))
            return {machoSym("om1_jump_resolve"), false, 0, 0};
        u64 base = reinterpret_cast<u64>(info.dli_saddr);
        if (t >= base && t - base < 0x1000000)
        {
            std::string fin = machoSym(info.dli_sname);
            if (t != base)
            {
                char ad[32];
                std::snprintf(ad, sizeof(ad), "+0x%llx", (unsigned long long)(t - base));
                fin += ad;
            }
            return {fin, false, 0, 0};
        }
    }
    return {"", false, 0, 0}; // unresolvable
}

struct PoolSlot
{
    u32 off = 0;
    u32 size = 0; // 4/8/16
    u64 value = 0; // low 8 bytes (16B slots: value2 holds high)
    u64 value2 = 0;
    SymRef sym;
    std::string label;
};

// LDR-literal scan: (w & 0x3B000000) == 0x18000000, minus PRFM (0xD8/0xF8).
bool scanPools(const Episode& e, std::map<u32, PoolSlot>& out)
{
    bool ok = true;
    for (u32 o = 0; o + 4 <= e.bytes.size(); o += 4)
    {
        u32 w = wordAt(e, o);
        if ((w & 0x3B000000u) != 0x18000000u)
            continue;
        u32 top = w >> 24;
        if (top == 0xD8 || top == 0xF8 || top == 0xDC || top == 0xFC)
            continue; // PRFM (literal): no load, no slot
        u32 size = 0;
        if (top == 0x18 || top == 0x1C || top == 0x98)
            size = 4;
        else if (top == 0x58 || top == 0x5C)
            size = 8;
        else if (top == 0x9C)
            size = 16;
        else
        {
            noteError("chunk %u+%x: unknown LDR-literal top byte %02x", e.id, o, top);
            ok = false;
            continue;
        }
        s64 imm = signExtend((w >> 5) & 0x7FFFFu, 19);
        s64 tgt = (s64)(e.base + o) + imm * 4;
        if (tgt < (s64)e.base || tgt + size > (s64)(e.base + e.bytes.size()) ||
            (tgt & 3))
        {
            noteError("chunk %u+%x: pool target %llx out of chunk", e.id, o,
                      (unsigned long long)tgt);
            ok = false;
            continue;
        }
        u32 toff = (u32)(tgt - (s64)e.base);
        auto it = out.find(toff);
        if (it != out.end())
        {
            if (it->second.size != size)
            {
                noteError("chunk %u: pool slot %x size conflict %u vs %u", e.id,
                          toff, it->second.size, size);
                ok = false;
            }
            continue;
        }
        PoolSlot s;
        s.off = toff;
        s.size = size;
        std::memcpy(&s.value, e.bytes.data() + toff, size > 8 ? 8 : size);
        if (size == 16)
            std::memcpy(&s.value2, e.bytes.data() + toff + 8, 8);
        char lb[64];
        std::snprintf(lb, sizeof(lb), "Lom1p_%u_%x", e.id, toff);
        s.label = lb;
        out[toff] = s;
    }
    // Symbolize slot values (pools here hold addresses).
    for (auto& kv : out)
    {
        PoolSlot& s = kv.second;
        if (s.size == 8)
        {
            s.sym = resolveData(s.value);
            if (s.sym.text.empty())
            {
                noteError("chunk %u pool+%x: unresolvable value %llx", e.id, s.off,
                          (unsigned long long)s.value);
                ok = false;
            }
        }
        else
        {
            noteError("chunk %u pool+%x: unexpected %u-byte slot (value %llx)", e.id,
                      s.off, s.size, (unsigned long long)s.value);
            ok = false;
        }
    }
    return ok;
}

// A branch to exactly the chunk end ("fall-through to next session"): the
// compiler bound a forward vixl label at session end; the next session's
// first block lands there (modulo alignment filler). Rewritten as a symbolic
// branch to the next chunk's label. Collected here, applied at emission.
struct FallThrough
{
    u32 off = 0;
    u8 kind = 0; // 0=B 1=BL 2=B.cond 3=CBZ 4=CBNZ 5=TBZ 6=TBNZ 7=ADR
    u32 w = 0;
};

// Safety nets: every control-flow/absolute word must be intra-chunk,
// recorded, or a fall-through to exactly the chunk end.
bool safetyNets(const Episode& e, const std::set<u32>& recOffs,
                std::map<u32, FallThrough>& falls)
{
    bool ok = true;
    u64 lo = e.base, hi = e.base + e.bytes.size();
    auto inChunk = [&](u64 t) { return t >= lo && t < hi; };
    auto atEnd = [&](u64 t) { return t == hi; };
    for (u32 o = 0; o + 4 <= e.bytes.size(); o += 4)
    {
        u32 w = wordAt(e, o);
        // B / BL
        if ((w & 0xFC000000u) == 0x14000000u || (w & 0xFC000000u) == 0x94000000u)
        {
            s64 imm = signExtend(w & 0x3FFFFFFu, 26);
            u64 t = e.base + o + (u64)(imm * 4);
            if (!inChunk(t) && !recOffs.count(o))
            {
                if (atEnd(t))
                    falls[o] = {o, (u8)(((w & 0xFC000000u) == 0x94000000u) ? 1 : 0), w};
                else
                {
                    noteError("chunk %u+%x: B/BL to %llx without record", e.id, o,
                              (unsigned long long)t);
                    ok = false;
                }
            }
        }
        // B.cond
        else if ((w & 0xFF000000u) == 0x54000000u)
        {
            s64 imm = signExtend((w >> 5) & 0x7FFFFu, 19);
            u64 t = e.base + o + (u64)(imm * 4);
            if (!inChunk(t))
            {
                if (atEnd(t))
                    falls[o] = {o, 2, w};
                else
                {
                    noteError("chunk %u+%x: B.cond escapes chunk to %llx", e.id, o,
                              (unsigned long long)t);
                    ok = false;
                }
            }
        }
        // CBZ/CBNZ
        else if ((w & 0x7F000000u) == 0x34000000u || (w & 0x7F000000u) == 0x35000000u)
        {
            s64 imm = signExtend((w >> 5) & 0x7FFFFu, 19);
            u64 t = e.base + o + (u64)(imm * 4);
            if (!inChunk(t))
            {
                if (atEnd(t))
                    falls[o] = {o, (u8)(((w & 0x7F000000u) == 0x35000000u) ? 4 : 3), w};
                else
                {
                    noteError("chunk %u+%x: CBZ/CBNZ escapes chunk to %llx", e.id, o,
                              (unsigned long long)t);
                    ok = false;
                }
            }
        }
        // TBZ/TBNZ: bits[30:24] = 110100x (bit31 is b5 of the bit number)
        else if ((w & 0x7F000000u) == 0x68000000u || (w & 0x7F000000u) == 0x69000000u)
        {
            s64 imm = signExtend((w >> 5) & 0x3FFFu, 14);
            u64 t = e.base + o + (u64)(imm * 4);
            if (!inChunk(t))
            {
                if (atEnd(t))
                    falls[o] = {o, (u8)(((w & 0x7F000000u) == 0x69000000u) ? 6 : 5), w};
                else
                {
                    noteError("chunk %u+%x: TBZ/TBNZ escapes chunk to %llx", e.id, o,
                              (unsigned long long)t);
                    ok = false;
                }
            }
        }
        // ADR (in-page PC-relative; must stay in chunk)
        else if ((w & 0x9F000000u) == 0x10000000u)
        {
            s64 imm = signExtend(((w >> 29) & 3u) | ((w >> 3) & 0x1FFFFCu), 21);
            u64 t = e.base + o + (u64)imm;
            if (!inChunk(t))
            {
                if (atEnd(t))
                    falls[o] = {o, 7, w};
                else
                {
                    noteError("chunk %u+%x: ADR escapes chunk to %llx", e.id, o,
                              (unsigned long long)t);
                    ok = false;
                }
            }
        }
        // ADRP must always have a record (else FAIL: unexpected absolute)
        else if ((w & 0x9F000000u) == 0x90000000u)
        {
            if (!recOffs.count(o))
            {
                noteError("chunk %u+%x: ADRP without record", e.id, o);
                ok = false;
            }
        }
    }
    return ok;
}

const char* condName(u32 c)
{
    static const char* n[16] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                                "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
    return n[c & 15];
}

} // namespace

// ---- .s + tables + manifest writer (Design A: layout-preserving) ----
bool Write(const char* dir, const char* tag)
{
    // P3.4: Mach-O final form is emitted directly (no post-processing).
    // P = "om1" + UPPER(tag): _om1b_ -> _om1SP1b_, om1_blocks -> om1SP1_blocks.
    // Shared consumer symbols (om1_jump_resolve, om1_fallthrough_miss) and the
    // trap trampoline stay un-namespaced. C decls use Mach-O linkage directly
    // (extern void om1SP1b_0 = asm _om1SP1b_0).
    std::string up = tag;
    for (char& c : up)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const std::string P = "om1" + up;
    // End-of-run range refresh: every compile has finished, so every live
    // program's ranges are closed. (BlockCompiled's hook fires at first-pass
    // init, when the current range is still open with end=-1.) Programs
    // evicted by a mid-run cache reset keep their last-wins ranges; the
    // consumer validates all ranges at init and fails loud on bad ones.
    {
        microVU* m = vuPtr(g_vu);
        std::map<std::pair<u64, u64>, microProgram*> live;
        for (auto& kv : m->mvuContentMap)
            live[std::make_pair(kv.first.low64, kv.first.high64)] = kv.second;
        for (microProgram* prog : m->mvuOrphanedProgs)
            live[std::make_pair(prog->contentHash.low64, prog->contentHash.high64)] = prog;
        for (auto& kv : g_progs)
        {
            auto it = live.find(kv.first);
            if (it == live.end() || !it->second->ranges)
                continue;
            kv.second.data.assign(
                reinterpret_cast<u8*>(it->second->data),
                reinterpret_cast<u8*>(it->second->data) + sizeof(it->second->data));
            kv.second.ranges.clear();
            for (const microRange& r : *it->second->ranges)
                kv.second.ranges.emplace_back(static_cast<u16>(r.start),
                                              static_cast<u16>(r.end));
        }
    }
    g_codeLabels.clear();
    // Block + stub labels.
    for (const BlockRec& b : g_blocks)
    {
        char lb[64];
        std::snprintf(lb, sizeof(lb), "_%sb_%u", P.c_str(), b.id);
        g_codeLabels[{b.gen, b.entryAddr}] = lb;
    }
    for (const Episode& e : g_episodes)
        for (const StubSnap& s : e.stubs)
        {
            char lb[96];
            std::snprintf(lb, sizeof(lb), "_%ss_%s_g%u", P.c_str(), s.name.c_str(), e.gen);
            auto key = std::make_pair(e.gen, s.addr);
            if (!g_codeLabels.count(key))
                g_codeLabels[key] = lb;
        }

    struct OutFix
    {
        u32 chunk = 0;
        u32 off = 0;
        FixKind kind;
        u8 reg = 0;
        bool isCall = false;
        u64 target = 0;
        std::string sym;  // resolved operand
        bool initSlot = false;
        u32 slotBlock = 0, slotField = 0;
        u32 gen = 0;
    };
    std::vector<OutFix> outs;
    std::map<u32, std::map<u32, PoolSlot>> pools; // chunk -> (off -> slot)
    std::map<u32, std::set<u32>> recOffs;         // chunk -> recorded word offsets
    std::map<u32, std::map<u32, FallThrough>> fallMap; // chunk -> (off -> fall)
    bool ok = true;

    // Pass 1: resolve fixups, scan pools, run safety nets.
    for (const Episode& e : g_episodes)
    {
        for (const Fixup& f : e.fixups)
        {
            OutFix o;
            o.chunk = e.id;
            o.off = f.off;
            o.kind = f.kind;
            o.isCall = f.isCall;
            o.target = f.target;
            o.gen = e.gen;
            if (f.heapBlock)
            {
                o.initSlot = true;
                o.slotBlock = f.blockIdx;
                o.slotField = f.fieldOff;
                char lb[64];
                std::snprintf(lb, sizeof(lb), "_%sh_%u_%x", P.c_str(), f.blockIdx, f.fieldOff);
                o.sym = lb; // consumer-filled pointer (dedup by name at emit)
            }
            else
            {
                auto it = g_codeLabels.find({e.gen, f.target});
                if (it != g_codeLabels.end())
                    o.sym = it->second;
                else
                {
                    SymRef r = resolveData(f.target);
                    if (r.text.empty())
                    {
                        Dl_info info;
                        const char* dl = (dladdr(reinterpret_cast<const void*>(f.target), &info) &&
                                          info.dli_sname)
                                             ? info.dli_sname
                                             : "(no dladdr)";
                        noteError("chunk %u+%x kind %d: unresolvable target %llx %s", e.id,
                                  f.off, (int)f.kind, (unsigned long long)f.target, dl);
                        ok = false;
                        continue;
                    }
                    o.sym = r.text;
                }
            }
            outs.push_back(o);
            // Mark covered words.
            u32 words = (f.kind == FK_Branch26) ? 1 : 4;
            for (u32 k = 0; k < words; ++k)
                recOffs[e.id].insert(f.off + k * 4);
            if (f.kind == FK_AbsCall)
                recOffs[e.id].insert(f.off + 16); // the blr/br word
            if (f.kind == FK_Adrp)
                recOffs[e.id].insert(f.off + 4); // the Add/Orr word
        }
        std::map<u32, PoolSlot> ps;
        if (!scanPools(e, ps))
            ok = false;
        pools[e.id] = ps;
        std::map<u32, FallThrough> fm;
        if (!safetyNets(e, recOffs[e.id], fm))
            ok = false;
        fallMap[e.id] = fm;
    }
    // Validate fall-throughs: the next episode must continue the same
    // generation within an alignment gap (< 16 bytes). A fall-through with
    // no next chunk (dangling edge at end of recording: never taken in this
    // replay) routes to the miss trap instead of failing.
    u32 nfalls = 0, ntraps = 0;
    std::set<std::pair<u32, u32>> trapFalls; // (chunk,off) routed to trap
    for (const Episode& e : g_episodes)
    {
        if (fallMap[e.id].empty())
            continue;
        u64 end = e.base + e.bytes.size();
        bool contiguous = false;
        if (e.id + 1 < g_episodes.size())
        {
            const Episode& nx = g_episodes[e.id + 1];
            contiguous = (nx.gen == e.gen && nx.base >= end && nx.base - end < 16);
        }
        if (!contiguous)
        {
            for (const auto& kv : fallMap[e.id])
            {
                if (kv.second.kind == 7) // ADR-to-trap unrepresentable; fail loud
                {
                    noteError("chunk %u+%x: dangling ADR fall-through", e.id, kv.first);
                    ok = false;
                }
                else
                {
                    trapFalls.insert({e.id, kv.first});
                    ++ntraps;
                }
            }
            std::fprintf(stderr, "[om1] chunk %u: %u fall-through(s) -> trap (no contiguous next)\n",
                         e.id, (unsigned)fallMap[e.id].size());
            continue;
        }
        nfalls += (u32)fallMap[e.id].size();
    }
    if (!ok || g_errors)
    {
        std::fprintf(stderr, "[om1] write aborted: %llu errors\n",
                     (unsigned long long)g_errors);
        return false;
    }

    // Pass 2: verify recorded-site shapes against the bytes.
    std::map<std::pair<u32, u32>, const OutFix*> fixAt; // (chunk,off) -> fix
    for (const OutFix& o : outs)
        fixAt[{o.chunk, o.off}] = &o;
    for (const Episode& e : g_episodes)
    {
        for (const Fixup& f : e.fixups)
        {
            if (f.kind == FK_AbsMov || f.kind == FK_AbsCall)
            {
                u64 v = 0;
                u32 rd = 0;
                for (int k = 0; k < 4; ++k)
                {
                    u32 w = wordAt(e, f.off + k * 4);
                    u32 want = (k == 0) ? 0xD2800000u : 0xF2800000u; // movz/movk
                    if ((w & 0xFF800000u) != want || ((w >> 21) & 3u) != (u32)k)
                    {
                        noteError("chunk %u+%x: bad canonical mov word %d (%08x)", e.id,
                                  f.off, k, w);
                        ok = false;
                        break;
                    }
                    if (k == 0)
                        rd = w & 31;
                    else if ((w & 31) != rd)
                    {
                        noteError("chunk %u+%x: mov reg mismatch", e.id, f.off);
                        ok = false;
                        break;
                    }
                    v |= (u64)((w >> 5) & 0xFFFFu) << (k * 16);
                }
                if (v != f.target)
                {
                    noteError("chunk %u+%x: mov target %llx != recorded %llx", e.id, f.off,
                              (unsigned long long)v, (unsigned long long)f.target);
                    ok = false;
                }
                // Fill in the dest reg for emission.
                const_cast<OutFix*>(fixAt[{e.id, f.off}])->reg = (u8)rd;
                if (f.kind == FK_AbsCall)
                {
                    u32 w = wordAt(e, f.off + 16);
                    bool isBlr = (w == 0xD63F0200u);
                    bool isBr = (w == 0xD61F0200u);
                    if (!isBlr && !isBr)
                    {
                        noteError("chunk %u+%x: AbsCall tail not blr/br x16 (%08x)", e.id,
                                  f.off, w);
                        ok = false;
                    }
                    if (isBlr != f.isCall)
                    {
                        noteError("chunk %u+%x: AbsCall kind/word mismatch", e.id, f.off);
                        ok = false;
                    }
                }
            }
            else if (f.kind == FK_Branch26)
            {
                u32 w = wordAt(e, f.off);
                u32 want = f.isCall ? 0x94000000u : 0x14000000u;
                if ((w & 0xFC000000u) != want)
                {
                    noteError("chunk %u+%x: expected %s, got %08x", e.id, f.off,
                              f.isCall ? "BL" : "B", w);
                    ok = false;
                }
                else
                {
                    s64 imm = signExtend(w & 0x3FFFFFFu, 26);
                    // AArch64 PC-relative addressing uses the instruction's
                    // own address (no ARMv7-style +4). The old +4 here (and in
                    // scanPools/safetyNets/the LDR rewrite) shifted every pool
                    // value by 4 bytes and misclassified fall-throughs; the
                    // bit-exact gate caught it (OM1 Part 2). +0 only now.
                    u64 t = e.base + f.off + (u64)(imm * 4);
                    {
                        static int ndbg2 = 0;
                        if (ndbg2++ == 0)
                        {
                            u32 prev = (f.off >= 4) ? wordAt(e, f.off - 4) : 0;
                            std::fprintf(stderr,
                                         "[om1-dbg] B+0=%08x B-4=%08x site=%llx rec=%llx\n", w,
                                         prev, (unsigned long long)(e.base + f.off),
                                         (unsigned long long)f.target);
                        }
                    }
                    if (t != f.target)
                    {
                        noteError("chunk %u+%x: branch decodes to %llx, recorded %llx",
                                  e.id, f.off, (unsigned long long)t,
                                  (unsigned long long)f.target);
                        ok = false;
                    }
                }
            }
            else if (f.kind == FK_Adrp)
            {
                u32 w0 = wordAt(e, f.off), w1 = wordAt(e, f.off + 4);
                if ((w0 & 0x9F000000u) != 0x90000000u)
                {
                    noteError("chunk %u+%x: expected ADRP, got %08x", e.id, f.off, w0);
                    ok = false;
                }
                if ((w1 & 0xFF000000u) != 0x91000000u && (w1 & 0xFF000000u) != 0xB2000000u &&
                    (w1 & 0xFF000000u) != 0x32000000u)
                {
                    noteError("chunk %u+%x: ADRP follower not Add/Orr (%08x)", e.id, f.off,
                              w1);
                    ok = false;
                }
                const_cast<OutFix*>(fixAt[{e.id, f.off}])->reg = (u8)(w0 & 31);
            }
        }
    }
    if (!ok || g_errors)
    {
        std::fprintf(stderr, "[om1] write aborted in verify: %llu errors\n",
                     (unsigned long long)g_errors);
        return false;
    }

    // Pass 3: emit the .s (layout-preserving: same word count per chunk).
    char spath[1024], tpath[1024], thpath[1024], mpath[1024], tripath[1024];
    std::snprintf(spath, sizeof(spath), "%s/%s.s", dir, tag);
    std::snprintf(tpath, sizeof(tpath), "%s/%s_tables.c", dir, tag);
    std::snprintf(thpath, sizeof(thpath), "%s/%s_tables.h", dir, tag);
    std::snprintf(mpath, sizeof(mpath), "%s/%s_manifest.txt", dir, tag);
    std::snprintf(tripath, sizeof(tripath), "%s/%s_triples.txt", dir, tag);
    FILE* s = std::fopen(spath, "wb");
    if (!s)
    {
        noteError("cannot open %s", spath);
        return false;
    }
    std::fprintf(s, "// OM1 offline microVU %s: %u chunks, %u blocks, %u programs.\n"
                    "// Layout-preserving: every chunk keeps its word count; intra-chunk\n"
                    "// PC-relative code is byte-valid. Pools live in __DATA (PIE-safe).\n",
                 tag, (unsigned)g_episodes.size(), (unsigned)g_blocks.size(),
                 (unsigned)g_progs.size());
    // Dedup init slots by name. Init quads use GLOBAL labels (the _om1h_
    // symbol itself): the consumer fills them from another TU, so file-local
    // L labels would not link.
    std::map<std::string, std::string> initQuad; // sym -> quad label (= sym)
    std::map<std::string, std::pair<u32, u32>> initInfo;
    u32 nquad = 0;
    for (const OutFix& o : outs)
        if (o.initSlot && !initQuad.count(o.sym))
        {
            initQuad[o.sym] = o.sym; // global label, shared with tables.c
            initInfo[o.sym] = {o.slotBlock, o.slotField};
        }
    // Pool quads for AbsMov/AbsCall/Adrp sites + literal pools.
    struct Quad
    {
        std::string label, sym;
    };
    std::vector<Quad> quads;
    std::map<std::pair<u32, u32>, std::string> siteQuad; // (chunk,off) -> quad
    for (const OutFix& o : outs)
    {
        if ((o.kind == FK_AbsMov || o.kind == FK_AbsCall || o.kind == FK_Adrp) && !o.initSlot)
        {
            char lb[64];
            std::snprintf(lb, sizeof(lb), "L%sq_%u", P.c_str(), nquad++);
            quads.push_back({lb, o.sym});
            siteQuad[{o.chunk, o.off}] = lb;
        }
    }
    for (const Episode& e : g_episodes)
        for (const auto& kv : pools[e.id])
        {
            char lb[64];
            std::snprintf(lb, sizeof(lb), "L%sq_%u", P.c_str(), nquad++);
            quads.push_back({lb, kv.second.sym.text});
            siteQuad[{e.id, kv.first}] = lb;
        }

    std::fprintf(s, "\t.text\n\t.align 4\n");
    std::set<u32> poolCovered; // per-chunk word offsets covered by pool slots
    for (const Episode& e : g_episodes)
    {
        std::fprintf(s, "\n// chunk %u: gen %u vu %d base %llx size %u fixups %u blocks %u\n",
                     e.id, e.gen, e.vu, (unsigned long long)e.base,
                     (unsigned)e.bytes.size(), (unsigned)e.fixups.size(),
                     (unsigned)e.blockIds.size());
        std::fprintf(s, "_%sc_%u:\n", P.c_str(), e.id);
        // Labels at block entries + stubs inside this chunk.
        std::map<u32, std::vector<std::string>> labels;
        for (u32 id : e.blockIds)
        {
            const BlockRec& b = g_blocks[id];
            char lb[64];
            std::snprintf(lb, sizeof(lb), "_%sb_%u", P.c_str(), id);
            labels[b.entryOff].push_back(lb);
        }
        for (const StubSnap& st : e.stubs)
        {
            if (st.addr >= e.base && st.addr < e.base + e.bytes.size())
            {
                char lb[96];
                std::snprintf(lb, sizeof(lb), "_%ss_%s_g%u", P.c_str(), st.name.c_str(), e.gen);
                labels[(u32)(st.addr - e.base)].push_back(lb);
            }
        }
        poolCovered.clear();
        for (const auto& kv : pools[e.id])
            for (u32 k = 0; k < kv.second.size; k += 4)
                poolCovered.insert(kv.first + k);
        for (u32 o = 0; o < e.bytes.size(); o += 4)
        {
            auto li = labels.find(o);
            if (li != labels.end())
                for (const auto& lb : li->second)
                    std::fprintf(s, "%s:\n", lb.c_str());
            auto fli = fallMap[e.id].find(o);
            if (fli != fallMap[e.id].end())
            {
                // Fall-through to the next chunk's base (see safetyNets),
                // or the miss trap when dangling (never taken in this replay).
                const FallThrough& ft = fli->second;
                char nxt[64];
                if (trapFalls.count({e.id, o}))
                {
                    // Conditional forms (B.cond/cbz/cbnz/tbz/tbnz) cannot
                    // reach the external trap symbol; route them through the
                    // file-local trampoline. Unconditional/ADR go direct.
                    std::snprintf(nxt, sizeof(nxt), "%s",
                                  (ft.kind >= 2 && ft.kind <= 6) ? "Lom1_trap_tramp"
                                                                : "_om1_fallthrough_miss");
                }
                else
                    std::snprintf(nxt, sizeof(nxt), "_%sc_%u", P.c_str(), e.id + 1);
                u32 rt = ft.w & 31;
                bool is64 = (ft.w >> 31) & 1;
                switch (ft.kind)
                {
                    case 0: std::fprintf(s, "\tb %s // was %08x (fall-through)\n", nxt, ft.w); break;
                    case 1: std::fprintf(s, "\tbl %s // was %08x (fall-through!)\n", nxt, ft.w); break;
                    case 2: std::fprintf(s, "\tb.%s %s // was %08x (fall-through)\n",
                                        condName(ft.w & 15), nxt, ft.w);
                        break;
                    case 3:
                    case 4:
                        std::fprintf(s, "\t%s %s%u, %s // was %08x (fall-through)\n",
                                     ft.kind == 3 ? "cbz" : "cbnz", is64 ? "x" : "w", rt,
                                     nxt, ft.w);
                        break;
                    case 5:
                    case 6:
                    {
                        u32 bit = ((ft.w >> 31) << 5) | ((ft.w >> 19) & 31);
                        std::fprintf(s, "\t%s %s%u, #%u, %s // was %08x (fall-through)\n",
                                     ft.kind == 5 ? "tbz" : "tbnz", is64 ? "x" : "w", rt,
                                     bit, nxt, ft.w);
                        break;
                    }
                    case 7:
                        std::fprintf(s, "\tadr x%u, %s // was %08x (fall-through)\n", rt,
                                     nxt, ft.w);
                        break;
                }
                continue;
            }
            auto fi = fixAt.find({e.id, o});
            if (fi != fixAt.end())
            {
                const OutFix* of = fi->second;
                if (of->kind == FK_Branch26)
                {
                    std::fprintf(s, "\t%s %s // was %08x\n", of->isCall ? "bl" : "b",
                                 of->sym.c_str(), wordAt(e, o));
                }
                else if (of->kind == FK_Adrp)
                {
                    const std::string& q = siteQuad[{e.id, o}];
                    std::fprintf(s, "\tadrp x%u, %s@PAGE\n\tldr x%u, [x%u, %s@PAGEOFF]\n",
                                 of->reg, q.c_str(), of->reg, of->reg, q.c_str());
                    o += 4; // consumes the Add/Orr word too
                }
                else // AbsMov / AbsCall
                {
                    std::string q;
                    if (of->initSlot)
                        q = initQuad[of->sym];
                    else
                        q = siteQuad[{e.id, o}];
                    std::fprintf(s, "\tadrp x%u, %s@PAGE\n\tldr x%u, [x%u, %s@PAGEOFF]\n\tnop\n\tnop\n",
                                 of->reg, q.c_str(), of->reg, of->reg, q.c_str());
                    o += 12; // consumes 4 words
                    if (of->kind == FK_AbsCall)
                    {
                        u32 w = wordAt(e, o + 4);
                        bool isBlr = (w == 0xD63F0200u);
                        std::fprintf(s, "\t%s x16\n", isBlr ? "blr" : "br");
                        o += 4;
                    }
                }
                continue;
            }
            if (poolCovered.count(o))
            {
                // Coalesce the run into one .space (keeps offsets inspectable).
                u32 run = 4;
                while (o + run < e.bytes.size() && poolCovered.count(o + run) &&
                       !labels.count(o + run) && !fixAt.count({e.id, o + run}))
                    run += 4;
                std::fprintf(s, "\t.space %u // pooled data (now in __DATA)\n", run);
                o += run - 4;
                continue;
            }
            // LDR-literal referencing a moved pool slot -> rewrite symbolically.
            u32 w = wordAt(e, o);
            bool rewrote = false;
            if ((w & 0x3B000000u) == 0x18000000u)
            {
                u32 top = w >> 24;
                if (top != 0xD8 && top != 0xF8 && top != 0xDC && top != 0xFC)
                {
                    s64 imm = signExtend((w >> 5) & 0x7FFFFu, 19);
                    u64 tgt = e.base + o + (u64)(imm * 4);
                    u32 toff = (u32)(tgt - e.base);
                    auto psi = pools[e.id].find(toff);
                    if (psi != pools[e.id].end())
                    {
                        u32 rt = w & 31;
                        bool is64 = (top == 0x58 || top == 0x5C);
                        bool isSimd = (top == 0x1C || top == 0x5C || top == 0x9C);
                        const std::string& q = siteQuad[{e.id, toff}];
                        if (!isSimd)
                            std::fprintf(s, "\tldr %s%u, %s // was %08x\n",
                                         is64 ? "x" : "w", rt, q.c_str(), w);
                        else
                        {
                            noteError("chunk %u+%x: SIMD LDR-literal to pool (unhandled)", e.id, o);
                            ok = false;
                        }
                        rewrote = true;
                    }
                }
            }
            if (!rewrote)
                std::fprintf(s, "\t.word 0x%08x\n", w);
        }
    }
    // __DATA: pool quads + init slots.
    std::fprintf(s, "\n\t.data\n\t.align 3\n");
    for (const Quad& q : quads)
        std::fprintf(s, "%s:\n\t.quad %s\n", q.label.c_str(), q.sym.c_str());
    for (const auto& kv : initQuad)
        std::fprintf(s, "\t.globl %s\n%s: // (consumer-filled)\n\t.quad 0\n",
                     kv.second.c_str(), kv.second.c_str());
    for (const Episode& e : g_episodes)
        std::fprintf(s, "\t.globl _%sc_%u\n", P.c_str(), e.id);
    for (const BlockRec& b : g_blocks)
        std::fprintf(s, "\t.globl _%sb_%u\n", P.c_str(), b.id);
    {
        std::set<std::string> done;
        for (const Episode& e : g_episodes)
            for (const StubSnap& st : e.stubs)
            {
                char lb[96];
                std::snprintf(lb, sizeof(lb), "_%ss_%s_g%u", P.c_str(), st.name.c_str(), e.gen);
                if (done.insert(lb).second)
                    std::fprintf(s, "\t.globl %s\n", lb);
            }
    }
    // Conditional-trap trampoline (see the fall-through emit above):
    // unconditional, so the symbol is always defined even when no trap
    // fall-through was recorded. File-local (L): no namespacing needed.
    std::fprintf(s, "\t.text\nLom1_trap_tramp:\n\tb _om1_fallthrough_miss\n");
    std::fclose(s);
    if (!ok || g_errors)
        return false;

    // ---- tables.c/h ----
    FILE* tc = std::fopen(tpath, "wb");
    FILE* th = std::fopen(thpath, "wb");
    if (!tc || !th)
    {
        noteError("cannot open tables output");
        return false;
    }
    std::fprintf(th, "// OM1 %s consumer tables.\n#pragma once\n", tag);
    std::fprintf(th, "#include <stdint.h>\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n");
    std::fprintf(th, "typedef struct { uint64_t lo, hi; uint32_t startPC; uint8_t exact;\n"
                     "  uint8_t pState[96]; uint8_t pStateEnd[96]; uint8_t hasJC;\n"
                     "  void* code; uint32_t objIdx; } %s_block_t;\n",
                 P.c_str());
    std::fprintf(th, "typedef struct { uint64_t lo, hi; const uint8_t* code;\n"
                     "  const uint16_t* ranges; uint32_t nranges; } %s_image_t;\n",
                 P.c_str());
    std::fprintf(th, "typedef struct { const char* name; uint32_t block; uint32_t field; } %s_initslot_t;\n",
                 P.c_str());
    std::fprintf(th, "extern const %s_block_t %s_blocks[];\nextern const uint32_t %s_nblocks;\n",
                 P.c_str(), P.c_str(), P.c_str());
    std::fprintf(th, "extern const %s_image_t %s_images[];\nextern const uint32_t %s_nimages;\n",
                 P.c_str(), P.c_str(), P.c_str());
    std::fprintf(th, "extern const %s_initslot_t %s_initslots[];\nextern const uint32_t %s_ninitslots;\n",
                 P.c_str(), P.c_str(), P.c_str());
    std::fprintf(th, "extern void* %s_stub_resume(void);\nextern void* %s_stub_exit(void);\n", P.c_str(),
                 P.c_str());
    std::fprintf(th, "#ifdef __cplusplus\n}\n#endif\n");
    std::fprintf(tc, "// OM1 %s consumer tables (generated).\n#include \"%s_tables.h\"\n", tag, tag);
    // Images.
    u32 ii = 0;
    std::map<std::pair<u64, u64>, u32> imgIdx;
    for (const auto& kv : g_progs)
    {
        imgIdx[kv.first] = ii;
        const ProgRec& p = kv.second;
        std::fprintf(tc, "static const uint8_t %s_img%u_code[%u] = {", P.c_str(), ii,
                     (unsigned)p.data.size());
        for (size_t k = 0; k < p.data.size(); ++k)
        {
            if (!(k % 16))
                std::fprintf(tc, "\n ");
            std::fprintf(tc, " 0x%02x,", p.data[k]);
        }
        std::fprintf(tc, "\n};\n");
        std::fprintf(tc, "static const uint16_t %s_img%u_ranges[%u] = {", P.c_str(), ii,
                     (unsigned)(p.ranges.size() * 2 + 2));
        for (const auto& r : p.ranges)
            std::fprintf(tc, "%u,%u,", r.first, r.second);
        std::fprintf(tc, "0xffff,0xffff};\n");
        ++ii;
    }
    std::fprintf(tc, "const %s_image_t %s_images[] = {\n", P.c_str(), P.c_str());
    for (const auto& kv : g_progs)
    {
        u32 i = imgIdx[kv.first];
        std::fprintf(tc, " {0x%llxull,0x%llxull,%s_img%u_code,%s_img%u_ranges,%u},\n",
                     (unsigned long long)kv.first.first, (unsigned long long)kv.first.second,
                     P.c_str(), i, P.c_str(), i, (unsigned)kv.second.ranges.size());
    }
    std::fprintf(tc, "};\nconst uint32_t %s_nimages = %u;\n", P.c_str(), (unsigned)g_progs.size());
    // Blocks (record order = insertion order; consumer adds in this order).
    // Mach-O C linkage: no underscore (extern void om1SP1b_0 = asm _om1SP1b_0).
    for (const BlockRec& b : g_blocks)
        std::fprintf(tc, "extern void %sb_%u(void);\n", P.c_str(), b.id);
    for (const auto& kv : initQuad)
        std::fprintf(tc, "extern void* %s;\n", kv.second.c_str());
    for (const Episode& e : g_episodes)
        for (const StubSnap& st : e.stubs)
            std::fprintf(tc, "extern void %ss_%s_g%u(void);\n", P.c_str(), st.name.c_str(), e.gen);
    std::fprintf(tc, "const %s_block_t %s_blocks[] = {\n", P.c_str(), P.c_str());
    for (const BlockRec& b : g_blocks)
    {
        u32 img = imgIdx[{b.hashLo, b.hashHi}];
        std::fprintf(tc, " {0x%llxull,0x%llxull,0x%x,%u,{", (unsigned long long)b.hashLo,
                     (unsigned long long)b.hashHi, b.startPC, (unsigned)b.pState[0]);
        for (int k = 0; k < 96; ++k)
            std::fprintf(tc, "%u,", b.pState[k]);
        std::fprintf(tc, "},{");
        for (int k = 0; k < 96; ++k)
            std::fprintf(tc, "%u,", b.pStateEnd[k]);
        std::fprintf(tc, "},%u,(void*)&%sb_%u,%u},\n", b.hasJC ? 1 : 0, P.c_str(), b.id, img);
    }
    std::fprintf(tc, "};\nconst uint32_t %s_nblocks = %u;\n", P.c_str(), (unsigned)g_blocks.size());
    // Init slots.
    std::fprintf(tc, "const %s_initslot_t %s_initslots[] = {\n", P.c_str(), P.c_str());
    for (const auto& kv : initQuad)
    {
        auto it = initInfo[kv.first];
        std::fprintf(tc, " {\"%s\",%u,%u},\n", kv.second.c_str(), it.first, it.second);
    }
    std::fprintf(tc, "};\nconst uint32_t %s_ninitslots = %u;\n", P.c_str(), (unsigned)initQuad.size());
    // Stub entry points (last generation wins).
    std::map<std::string, std::string> stubPick;
    for (const Episode& e : g_episodes)
        for (const StubSnap& st : e.stubs)
        {
            char lb[96];
            std::snprintf(lb, sizeof(lb), "_%ss_%s_g%u", P.c_str(), st.name.c_str(), e.gen);
            stubPick[st.name] = lb;
        }
    {
        auto it = stubPick.find("startFunctResume");
        std::fprintf(tc, "void* %s_stub_resume(void) { return (void*)&%s; }\n", P.c_str(),
                     it != stubPick.end() ? it->second.c_str() + 1 : "0");
        it = stubPick.find("exitFunct");
        std::fprintf(tc, "void* %s_stub_exit(void) { return (void*)&%s; }\n", P.c_str(),
                     it != stubPick.end() ? it->second.c_str() + 1 : "0");
    }
    std::fclose(tc);
    std::fclose(th);

    // ---- manifest + triples ----
    FILE* m = std::fopen(mpath, "wb");
    FILE* tr = std::fopen(tripath, "wb");
    if (m)
    {
        std::fprintf(m, "tag=%s episodes=%u blocks=%u programs=%u resets=%u codeBytes=%llu\n",
                     tag, EpisodeCount(), BlockCount(), (unsigned)g_progs.size(), g_resets,
                     CodeBytes());
        for (const auto& kv : g_progs)
            std::fprintf(m, "program %016llx%016llx ranges=%u\n",
                         (unsigned long long)kv.first.second, (unsigned long long)kv.first.first,
                         (unsigned)kv.second.ranges.size());
        std::set<std::string> seen;
        for (const BlockRec& b : g_blocks)
        {
            u64 q64 = 0;
            std::memcpy(&q64, b.pState, 8);
            std::fprintf(m, "block id=%u gen=%u chunk=%u pc=%04x exact=%u hasJC=%u hash=%016llx%016llx q=%016llx\n",
                         b.id, b.gen, b.chunk, b.startPC, (unsigned)b.pState[0],
                         b.hasJC ? 1 : 0, (unsigned long long)b.hashHi,
                         (unsigned long long)b.hashLo, (unsigned long long)q64);
            char tk[128];
            std::snprintf(tk, sizeof(tk), "%016llx%016llx %04x %016llx %u",
                          (unsigned long long)b.hashHi, (unsigned long long)b.hashLo, b.startPC,
                          (unsigned long long)q64, (unsigned)b.pState[0]);
            if (tr && seen.insert(tk).second)
                std::fprintf(tr, "%s\n", tk);
        }
        std::fclose(m);
    }
    if (tr)
        std::fclose(tr);
    std::fprintf(stderr, "[om1] wrote %s.* : %u chunks %u blocks %u programs %u quads %u initslots %u fallthrough %u traps errors=%llu\n",
                 tag, EpisodeCount(), BlockCount(), (unsigned)g_progs.size(), nquad,
                 (unsigned)initQuad.size(), nfalls, ntraps, (unsigned long long)g_errors);
    return g_errors == 0;
}

} // namespace om1

