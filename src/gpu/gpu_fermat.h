// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Unified GPU Fermat Primality Test Dispatch Layer
 *
 * Provides a single API that selects the best available GPU backend:
 * - Metal (macOS ARM64 — native, zero-copy unified memory)
 * - OpenCL (Linux, Windows, macOS x86_64 — dynamic loading)
 *
 * Callers use gpu_fermat_*() functions and never need to know which
 * backend is active.
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#ifndef FREYCOIN_GPU_GPU_FERMAT_H
#define FREYCOIN_GPU_GPU_FERMAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** GPU backend type */
typedef enum {
    GPU_BACKEND_NONE = 0,
    GPU_BACKEND_METAL = 1,
    GPU_BACKEND_OPENCL = 2
} GpuBackendType;

/**
 * Initialize the best available GPU backend.
 * On macOS ARM64: tries Metal first, falls back to OpenCL.
 * On other platforms: OpenCL only.
 *
 * @param device_id Device index (0 for first GPU)
 * @return 0 on success, -1 on error, -2 if no GPU available
 */
int gpu_fermat_init(int device_id);

/**
 * Cleanup GPU resources for the active backend.
 */
void gpu_fermat_cleanup(void);

/**
 * Run batch Fermat primality test on the active GPU backend.
 * Tests if 2^(p-1) == 1 (mod p) for each prime candidate.
 *
 * @param h_results Output array: 1 = probably prime, 0 = composite
 * @param h_primes  Input array of candidates (limb-packed format)
 * @param count     Number of candidates to test
 * @param bits      Bit size: 320 or 352
 * @return 0 on success, -1 on error
 */
int gpu_fermat_batch(uint8_t *h_results, const uint32_t *h_primes,
                     uint32_t count, int bits);

/**
 * Get number of available GPU devices (for active backend).
 * @return Number of GPUs, 0 if none
 */
int gpu_get_device_count(void);

/**
 * Get device name string.
 * @param device_id Device index
 * @return Device name (static buffer, do not free)
 */
const char* gpu_get_device_name(int device_id);

/**
 * Get device memory size.
 * @param device_id Device index
 * @return Memory size in bytes
 */
size_t gpu_get_device_memory(int device_id);

/**
 * Check if any GPU backend is available.
 * @return 1 if available, 0 if not
 */
int gpu_is_available(void);

/**
 * Get the currently active backend type.
 * @return GpuBackendType enum value
 */
GpuBackendType gpu_get_active_backend(void);

/**
 * Get a human-readable name for the active backend.
 * @return "Metal", "OpenCL", or "None"
 */
const char* gpu_get_backend_name(void);

#ifdef __cplusplus
}
#endif

#endif // FREYCOIN_GPU_GPU_FERMAT_H
