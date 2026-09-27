# ssx3-ge1: PCSX2's GS as an external renderer for the SSX 3 static-recompilation runtime

This branch is upstream PCSX2 `ae2bac2b09a7622e5181dc7bf5f6b521c53f7b89`. The GS tree is unmodified apart from a small
listed patch set:
- a public timing accessor;
- Android AHB export/fence entry points and GPU-side snapshot composition;
- an optional Turnip HAL loader seam;
- static shaderc on Android.

`ssx3-ge1/adapter/` is a narrow C ABI (`ge1_gs_*`) that exposes the GS to the PS2Recomp runtime's external GS backend
(`PS2X_GS_BACKEND=external`), plus an offline replayer for the runtime's GS stream captures.
`ssx3-ge1/android-platform/` builds it for Android arm64. It follows ARMSX2's Android CMake/dependency layout as a
platform reference; no ARMSX2 GS behaviour or performance policies are imported.

GPL-3.0-or-later, like PCSX2. No game data, generated code or captures live here.
