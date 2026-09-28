# microVU source of record (ssx3)

Branch `armsx2-ssx3` of `brad-richardson/pcsx2`. It holds the ARMSX2
microVU sources that the ssx3 project's shipped libraries build from,
on top of real upstream ARMSX2 history. GPL-3.0+, like upstream.

## History

| Commit | Content | Product |
| --- | --- | --- |
| `247fa6f` (upstream ARMSX2) | Pristine base | — |
| `[MS1] MV1` | Path1 test hooks + `mv1_replay` bench | Odin bench (feasibility) |
| `[MS1] MV2` | `g_path1_complete` + `PS2X_MICROVU_EMBED` guards + `mv2` bridge | **`libmv2_microvu.so` (Odin play lib)** |
| `[MS1] OM1` ×5 | Route-a capture recorder + iOS platform root | **iOS static archives (iOS v2)** |

Lane reports: ssx3 `local/research/{MV1,MV2,OM1}/REPORT.md`. The MV2
commit matches the folded fork patch
(`PS2Recomp/ps2xRuntime/third_party/microvu/`: `armsx2.patch`,
`bridge/`, `platform/`) byte for byte; that directory stays the
runtime-side reference.

## What builds from which commit

- **Odin `libmv2_microvu.so`** — from the **MV2 commit**. Wrapper
  `ssx3/mv2-android-platform/` adds `common/`, `pcsx2/` (with
  `PS2X_MICROVU_EMBED=1`), `3rdparty/libretro` headers and
  `mv2-adapter/` (bridge + PCSX2 test-harness init). Frontend dirs
  (`pcsx2-qt`, `pcsx2-sdl`, …) are never referenced. Byte-match proven:
  rebuilt lib is SHA256-identical to the play APK's `jniLibs` copy
  (see MS1 report).
- **iOS archives** (`libPCSX2.a`, `libcommon.a` + 26 3rdparty) — from
  the **tip**. Root `om1-ios-platform/` adds `common/` + `pcsx2/`
  (Metal-only, `ENABLE_RECOMPILER_TEST_HOOKS=ON`) plus the in-tree iOS
  stubs. Byte-match proven: 28/28 archives (+1 symlink) identical to
  the OM1 shipped set.
- `ssx3/mv1-android-platform/` + `mv1-adapter/` reproduce the MV1
  feasibility bench; not shipped.

The wrappers keep their lane-relative paths (`../vendor/armsx2`,
`../armsx2`); rebuilds reconstruct the sibling layout below.

## Build 1: Odin lib (bradflix, `ssx3-android` Docker)

External inputs: the GE1 Android 3rdparty bundle
(`~/dev/ssx3-work/GE1/android-platform/3rdparty`, mounted read-only),
NDK 28.2.13676358, CMake 3.22.1, Ninja. Layout on the host:

```
MS1-proof/
  vendor/armsx2/      # git archive of the MV2 commit, minus ssx3/
  android-platform/   # ssx3/mv2-android-platform/ from the branch
  android-build/      # fresh output dir
```

```sh
MNT=/home/brad/dev/ssx3-work/MS1-proof
GE1=/home/brad/dev/ssx3-work/GE1/android-platform/3rdparty
IMG=ssx3-android   # e310655c7e0a
MEM="--memory=24g --memory-swap=24g --oom-score-adj=500"
CM=/opt/android-sdk/cmake/3.22.1/bin/cmake
TC=/opt/android-sdk/ndk/28.2.13676358/build/cmake/android.toolchain.cmake
EPOCH=$(date -u -d '2026-09-27 06:46:39' +%s)   # MV2's configure second

docker run --rm $MEM -v $MNT:/work -v $GE1:/work/android-platform/3rdparty:ro \
  -w /work/android-platform -e SOURCE_DATE_EPOCH=$EPOCH $IMG \
  $CM -S /work/android-platform -B /work/android-build -G Ninja \
  -DCMAKE_MAKE_PROGRAM=/opt/android-sdk/cmake/3.22.1/bin/ninja \
  -DCMAKE_TOOLCHAIN_FILE=$TC -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release \
  -DBUILD=20260927
docker run --rm $MEM -v $MNT:/work \
  -v $GE1:/work/android-platform/3rdparty:ro $IMG \
  $CM --build /work/android-build --target mv2_microvu -j6
docker run --rm --memory=4g --memory-swap=4g --oom-score-adj=500 \
  -v $MNT:/work $IMG \
  /opt/android-sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip \
  --strip-unneeded /work/android-build/mv2-adapter/libmv2_microvu.so \
  -o /work/libmv2_microvu.so
```

`-DBUILD` / `SOURCE_DATE_EPOCH` re-pin two configure-time date stamps
(libjpeg-turbo `BUILD`, SPIRV-Tools `build-version.inc`) to MV2's
values; everything else is MV2's recipe verbatim, at identical
container paths. Result: `d8a5785a…` (stripped, = play APK),
`dcb68242…` (unstripped).

## Build 2: iOS archives (mini)

External inputs: GI1's `ios-3rdparty` (`-DOM1_IOS_DEPS`), Homebrew
LLVM clang 23.1.1, Xcode iPhoneOS 27.0 SDK, the shared ccache
(`~/dev/ssx3-work/ccache-ios`, whose `base_dir` rewriting is part of
the byte recipe). The proof root must sit exactly two levels under
`ssx3-work` (like `OM1/`), because ccache rewrites paths relative to
the build dir:

```
MS1proof/
  armsx2/       # git archive of the tip
  ios-device/   # fresh output dir
```

```sh
PROOF=$HOME/dev/ssx3-work/MS1proof
export CCACHE_DIR=$HOME/dev/ssx3-work/ccache-ios
MAP="-ffile-prefix-map=$PROOF/armsx2=$HOME/dev/ssx3-work/OM1/armsx2"
MAP="$MAP -fdebug-prefix-map=$PROOF/armsx2=$HOME/dev/ssx3-work/OM1/armsx2"
cmake -S $PROOF/armsx2/om1-ios-platform -B $PROOF/ios-device -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release \
  -DOM1_IOS_DEPS=$HOME/dev/ssx3-work/GI1/ios-3rdparty \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DBUILD=20260927 "-DCMAKE_OBJCXX_FLAGS=$MAP"
cmake --build $PROOF/ios-device --target PCSX2 -j8
```

Notes (all verified by the MS1 byte-match):

- `-DBUILD=20260927` re-pins libjpeg-turbo's date stamp. No other
  date carriers exist in this build (Metal-only: no shaderc/SPIRV).
- The prefix map applies to **Objective-C++ only**: the two `.mm`
  files bypass ccache with absolute paths, while C/C++ get
  ccache-`base_dir`-relative paths — a global map shadows that
  rewriting and breaks the match.
- Exact `.a` bytes additionally need member-mtime normalization
  (BSD `ar` embeds them) plus the `__.SYMDEF` header date; see the
  MS1 report for the procedure. Member objects match without it.

## Hygiene

No game-derived content on this branch: no recorded `.s`/tables,
no `.vuprog`/`.om1` captures, no bench record outputs. The OM1
bench adapters (`om1-adapter/`, `om1bench-adapter/`,
`om1rt-adapter/`) and their root-CMake lines were deliberately left
out with the P7-2 inventory; the record outputs never lived in the
source tree.
