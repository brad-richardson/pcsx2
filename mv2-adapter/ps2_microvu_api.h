// SPDX-License-Identifier: GPL-3.0-or-later
// MV2: the only ABI shared with the separately linked ARMSX2 microVU library.
#pragma once

#include <stdint.h>

#define PS2X_MICROVU_ABI 1u
#if defined(__GNUC__)
#define PS2X_MV2_EXPORT __attribute__((visibility("default")))
#else
#define PS2X_MV2_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps2x_microvu_state {
    uint32_t vf[32][4];
    uint32_t vi[16];
    uint32_t acc[4];
    uint32_t q, p, i, r;
    uint32_t pc, mac, clip, status;
    uint64_t cycles;
    uint32_t top, itop;
    uint32_t stopped_d, stopped_t, budget_exhausted;
} ps2x_microvu_state;

typedef void (*ps2x_microvu_path1_fn)(void *opaque, const uint8_t *bytes, uint32_t size);

// One caller owns the library, and all calls must run on the existing MTVU
// worker. Error strings are static. The library never starts an EE, GIF or VU
// scheduler thread.
PS2X_MV2_EXPORT uint32_t ps2x_microvu_abi(void);
PS2X_MV2_EXPORT int ps2x_microvu_init(const char **error);
PS2X_MV2_EXPORT void ps2x_microvu_shutdown(void);
PS2X_MV2_EXPORT int ps2x_microvu_run(const uint8_t *code, uint32_t code_size, uint64_t generation,
                     uint8_t *data, uint32_t data_size, uint32_t start_pc,
                     uint32_t resume, uint32_t top, uint32_t itop,
                     uint32_t fbrst, uint32_t budget,
                     ps2x_microvu_state *state,
                     ps2x_microvu_path1_fn path1, void *path1_opaque,
                     const char **error);

#ifdef __cplusplus
}
#endif
