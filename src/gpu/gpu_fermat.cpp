// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Unified GPU Fermat Primality Test Dispatch Implementation
 *
 * Selects Metal or OpenCL at runtime based on platform and availability.
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#include "gpu_fermat.h"
#include "opencl_fermat.h"

#ifdef __APPLE__
#include "metal_fermat.h"
#endif

#include <stdio.h>

static GpuBackendType g_active_backend = GPU_BACKEND_NONE;

int gpu_fermat_init(int device_id) {
#ifdef __APPLE__
#if defined(__aarch64__) || defined(__arm64__)
    /* Apple Silicon: prefer Metal (native, zero-copy unified memory) */
    if (metal_fermat_init(device_id) == 0) {
        g_active_backend = GPU_BACKEND_METAL;
        fprintf(stderr, "GPU: Metal backend initialized (%s)\n",
                metal_get_device_name(device_id));
        return 0;
    }
    fprintf(stderr, "GPU: Metal init failed, trying OpenCL fallback...\n");
#endif
    /* macOS x86_64 or Metal failed: try OpenCL (discrete AMD GPUs, etc.) */
    if (opencl_fermat_init(device_id) == 0) {
        g_active_backend = GPU_BACKEND_OPENCL;
        fprintf(stderr, "GPU: OpenCL backend initialized (%s)\n",
                opencl_get_device_name(device_id));
        return 0;
    }
#else
    /* Linux / Windows: OpenCL only */
    if (opencl_fermat_init(device_id) == 0) {
        g_active_backend = GPU_BACKEND_OPENCL;
        return 0;
    }
#endif

    g_active_backend = GPU_BACKEND_NONE;
    return -2;
}

void gpu_fermat_cleanup(void) {
    switch (g_active_backend) {
#ifdef __APPLE__
    case GPU_BACKEND_METAL:
        metal_fermat_cleanup();
        break;
#endif
    case GPU_BACKEND_OPENCL:
        opencl_fermat_cleanup();
        break;
    default:
        break;
    }
    g_active_backend = GPU_BACKEND_NONE;
}

int gpu_fermat_batch(uint8_t *h_results, const uint32_t *h_primes,
                     uint32_t count, int bits) {
    switch (g_active_backend) {
#ifdef __APPLE__
    case GPU_BACKEND_METAL:
        return metal_fermat_batch(h_results, h_primes, count, bits);
#endif
    case GPU_BACKEND_OPENCL:
        return opencl_fermat_batch(h_results, h_primes, count, bits);
    default:
        return -1;
    }
}

int gpu_get_device_count(void) {
    switch (g_active_backend) {
#ifdef __APPLE__
    case GPU_BACKEND_METAL:
        return metal_get_device_count();
#endif
    case GPU_BACKEND_OPENCL:
        return opencl_get_device_count();
    default:
        break;
    }

    /* Not initialized yet — probe available backends */
#ifdef __APPLE__
#if defined(__aarch64__) || defined(__arm64__)
    {
        int count = metal_get_device_count();
        if (count > 0) return count;
    }
#endif
#endif
    return opencl_get_device_count();
}

const char* gpu_get_device_name(int device_id) {
    switch (g_active_backend) {
#ifdef __APPLE__
    case GPU_BACKEND_METAL:
        return metal_get_device_name(device_id);
#endif
    case GPU_BACKEND_OPENCL:
        return opencl_get_device_name(device_id);
    default:
        return "N/A";
    }
}

size_t gpu_get_device_memory(int device_id) {
    switch (g_active_backend) {
#ifdef __APPLE__
    case GPU_BACKEND_METAL:
        return metal_get_device_memory(device_id);
#endif
    case GPU_BACKEND_OPENCL:
        return opencl_get_device_memory(device_id);
    default:
        return 0;
    }
}

int gpu_is_available(void) {
    if (g_active_backend != GPU_BACKEND_NONE) return 1;
    return gpu_get_device_count() > 0 ? 1 : 0;
}

GpuBackendType gpu_get_active_backend(void) {
    return g_active_backend;
}

const char* gpu_get_backend_name(void) {
    switch (g_active_backend) {
    case GPU_BACKEND_METAL:  return "Metal";
    case GPU_BACKEND_OPENCL: return "OpenCL";
    default:                 return "None";
    }
}
