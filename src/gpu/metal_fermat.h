// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Metal Fermat Primality Test Interface
 *
 * Native Apple Silicon GPU backend using Metal compute shaders.
 * Exploits unified memory for zero-copy data transfer between CPU and GPU.
 *
 * THREAD SAFETY: All functions in this API must be called from a single
 * thread. The mining engine's GPU worker thread serializes access; do not
 * call these functions concurrently from multiple threads.
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#ifndef FREYCOIN_GPU_METAL_FERMAT_H
#define FREYCOIN_GPU_METAL_FERMAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize Metal for Fermat primality testing.
 * @param device_id Device index (0 for default GPU)
 * @return 0 on success, -1 on error, -2 if Metal not available
 */
int metal_fermat_init(int device_id);

/**
 * Cleanup Metal resources.
 */
void metal_fermat_cleanup(void);

/**
 * Run batch Fermat primality test on GPU via Metal.
 * Tests if 2^(p-1) == 1 (mod p) for each prime candidate.
 *
 * Uses MTLResourceStorageModeShared for zero-copy on unified memory.
 *
 * @param h_results Output array: 1 = probably prime, 0 = composite
 * @param h_primes  Input array of candidates (limb-packed format)
 * @param count     Number of candidates to test
 * @param bits      Bit size: 320 or 352
 * @return 0 on success, -1 on error
 */
int metal_fermat_batch(uint8_t *h_results, const uint32_t *h_primes,
                       uint32_t count, int bits);

/**
 * Get number of available Metal GPU devices.
 * @return Number of Metal-capable GPUs, 0 if none
 */
int metal_get_device_count(void);

/**
 * Get device name string.
 * @param device_id Device index
 * @return Device name (static buffer, do not free)
 */
const char* metal_get_device_name(int device_id);

/**
 * Get device recommended max working set size (approximation of usable memory).
 * @param device_id Device index
 * @return Memory size in bytes
 */
size_t metal_get_device_memory(int device_id);

/**
 * Check if Metal is available on this system.
 * @return 1 if available, 0 if not
 */
int metal_is_available(void);

#ifdef __cplusplus
}
#endif

#endif // FREYCOIN_GPU_METAL_FERMAT_H
