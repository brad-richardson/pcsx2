# SSX3 carry on ARMSX2 master (AX4b)

Branch `armsx2-ssx3-one` = `armsx2/master` (`e23b6a55`, was `b3ab7f5a37` at
squash time) + the SSX 3 GS + microVU carry as 12 squashed fix groups, one
commit per group. Nothing here is rewritten: upstream bumps arrive as
`git merge armsx2/master`. When the carry needs a re-squash, cut `-v2`, …
with a new old→new mapping and freeze this branch.

Classes: **A** additive / out-of-tree-able. **B** a hook or API we need.
**C** a bug fix, upstream candidate (no contact until Brad says so).
**D** local policy or driver workaround. Full per-commit table: AX4
`local/research/AX4/REPORT.md` §1. Old→new map: `local/research/AX4b`
`receipts/mapping.tsv`. Additive dirs moved to the PS2Recomp fork
(`ps2xRuntime/third_party/armsx2/`); only in-tree carry + `om1/om1_hooks.h`
stay here.

| Group | Title | Class | Absorbs |
| --- | --- | --- | --- |
| SG01 | build: iOS/Android/Mac embed portability | A/C | 4 commits (BuildParameters, pcsx2/CMakeLists, `<cstdlib>`, test path) |
| SG02 | GS/Metal: surfaceless host + async IOSurface export | B/C | 3 commits |
| SG03 | GS/Vulkan Android: AHB export seam, export drains, bundled shaderc, external Vulkan HAL loader | B/D | 2 commits (279ec15d32 in-tree; 1feaed4bf6 drains) |
| SG04 | GS: export output filters (CAS, sharp bilinear) | B | f2c0eef010 in-tree |
| SG05 | GS: split-front state refresh under draw buffering | C | f03d4caead, 4f37b972ea |
| SG06 | GS: a PACKED resume that ends the chunk keeps the tag open (+ test) | C | a1435acbdb |
| SG07 | GS/Vulkan: Adreno destination-read roads | D | c17622bbf2, 48c2b45e7d, 1076c536fa, 1feaed4bf6 (over-one), e265383f31 |
| SG08 | GS: Adreno/Turnip blend workarounds (blend-mix divert; AFIX; FX2 rta kept) | D | 97878e8e42, 9c9d35fa0e, b780cd0b0a (logic) |
| SG09 | GS/Vulkan: TFX pipeline prewarm + pipeline-cache flush | B | 74898bbb95 (prewarm/flush part) |
| SG10 | GS: diagnostics (back-thread busy ms, pipeline/stall stats, GE7/FX2 counters) | B-diag | 9a9e3f3d70, f11e8e2aa9, 167d7ce1c8, 74898bbb95 (stats), b780cd0b0a (counters) |
| SG11 | microVU: embed hooks | B | ff54158581 + 4e6e8b04cd (in-tree parts) |
| SG12 | microVU/arm64: OM1 capture recorder | B (+A) | 7003289a65, 99ff22da28, c7896d0dc5 (root hook, optional) |

Kept, not dropped: FX2 `GE1_ADRENO_AD_ACCU=rta` (default off, in no play
env) folded into SG08 + SG10; SG10 diagnostics as-is (no diag macro).
Upstream-candidate flags: GE9 PACKED-resume fix (top), GS10 split-front
refresh, surfaceless Metal host, kgsl poll + dynamic-draw-state (Mesa
series), `<cstdlib>` includes. D-road policy follows
`GSSelfReadRoadPolicy` one-enum-value style (GE7 CopySplit).

Root `CMakeLists.txt`: the GE1 adapter hook is gone (adapter builds out of
tree); the OM1 hook is optional (`if(DEFINED OM1_CAPTURE_INCLUDE)`, never a
fatal error) — a plain configure works without OM1 inputs. Equivalence:
`git diff <ax4-one+new-master> armsx2-ssx3-one -- pcsx2 common 3rdparty cmake`
is empty; the root file shows only the removed hooks + the guarded OM1 hook.
