// OM1 route-a capture recorder: hook declarations for ARMSX2 lib call sites.
// OM1-private (lives in the OM1 scratch tree, never upstreamed as-is).
// Every hook is a no-op unless om1::Attach() installed the recorder, so the
// oracle binary (which never calls Attach) emits byte-identical JIT code.
// Uses stdint types only (the lib's u8/u32 may or may not be visible here).
#pragma once

#include <cstdint>

struct microVU;
struct microBlock;

namespace om1
{

// True while a capture recorder is attached (checked on emit paths).
bool Active();

// Attach a fresh recorder for VU vuIndex (0/1). Also installs it as the
// thread-local armAddressRecorder. Detach() uninstalls (keeps data).
void Attach(int vuIndex);
void Detach();

// Lib hook sites (see the call sites for exact placement):
void EpisodeBegin(int vuIndex, uint8_t* base); // code emission starting at base
void EpisodeEnd(int vuIndex, uint8_t* end);    // bytes [base,end) final (pool incl.)
void BlockCompiled(int vuIndex, microBlock* blk, uint8_t* entry, uint32_t startPC);
void OnReset(int vuIndex); // mVUreset ran (generation bump)

// ---- tool API (om1_record.cpp) ----

// Name a linked symbol for the .s symbolizer (exact address -> name).
void AddKnownSymbol(const void* addr, const char* name);

// Write tag.s, tag_tables.c/h, tag_manifest.txt, tag_triples.txt into dir.
// Returns false on any unresolvable reference (details to stderr).
bool Write(const char* dir, const char* tag);

// Stats for the report tables.
uint32_t EpisodeCount();
uint32_t BlockCount();   // episodes' blocks (with duplicates across resets)
uint32_t TripleCount();  // distinct (program,pc,pstate) triples
uint32_t ResetCount();
unsigned long long CodeBytes();

// Weak-linked kill switch for the offline consumer. The strong definition
// lives in the tool that needs it (om1_record.cpp / om1_consume.cpp); tools
// that don't link it see a null weak symbol and skip the check.
#if defined(__APPLE__) || defined(__linux__)
extern bool om1_no_codegen_flag __attribute__((weak));
inline bool CodegenForbidden() { return &om1_no_codegen_flag && om1_no_codegen_flag; }
#else
inline bool CodegenForbidden() { return false; }
#endif

} // namespace om1
