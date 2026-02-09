// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * CUDA Fermat Primality Test — Driver API Implementation
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 *
 * Uses the CUDA Driver API (nvcuda.dll / libcuda.so) loaded dynamically
 * at runtime. No CUDA Toolkit or nvcc needed at build time.
 *
 * PTX kernels are embedded as string constants and JIT-compiled by the
 * driver to the user's specific GPU architecture at initialization.
 * This is the same approach used by OpenCL (source → JIT compile),
 * and means a single binary works on all NVIDIA GPUs from Maxwell
 * (GTX 750 Ti, 2014) through current Ada Lovelace (RTX 4090).
 *
 * Architecture:
 *   fermat.cu → nvcc -ptx (dev machine) → fermat_ptx_source.h (committed)
 *   Build: cuda_fermat_driver.cpp compiled as plain C++ (no nvcc)
 *   Runtime: cuda_loader loads nvcuda.dll → cuModuleLoadData(ptx) → JIT
 */

#include "cuda_fermat.h"
#include "cuda_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Embedded PTX source (generated from fermat.cu by nvcc -ptx -arch=sm_50) */
static const char* ptx_source =
#include "fermat_ptx_source.h"
;

/* Thread block size — must match the BLOCK_SIZE in fermat.cu */
#define BLOCK_SIZE 64

/* Global CUDA Driver API state */
static CUdevice   g_device = -1;
static CUcontext  g_context = nullptr;
static CUmodule   g_module = nullptr;
static CUfunction g_kernel_320 = nullptr;
static CUfunction g_kernel_352 = nullptr;
static CUfunction g_kernel_selftest = nullptr;
static int        g_initialized = 0;
static char       g_device_name[256] = {0};
static size_t     g_device_memory = 0;

int cuda_fermat_init(int device_id) {
    CUresult res;

    if (g_initialized) return 0;

    /* Ensure CUDA Driver API is dynamically loaded */
    if (!cuda_is_loaded()) {
        if (cuda_load() != 0) {
            return -2;  /* CUDA driver not available */
        }
    }

    /* Get device count */
    int device_count = 0;
    res = cu_cuDeviceGetCount(&device_count);
    if (res != CUDA_SUCCESS || device_count == 0) {
        return -2;
    }

    /* Clamp device_id */
    if (device_id >= device_count) {
        device_id = 0;
    }

    /* Get device handle */
    res = cu_cuDeviceGet(&g_device, device_id);
    if (res != CUDA_SUCCESS) {
        return -1;
    }

    /* Query device info */
    cu_cuDeviceGetName(g_device_name, sizeof(g_device_name), g_device);
    cu_cuDeviceTotalMem(&g_device_memory, g_device);

    /* Check compute capability (require 5.0+ for our PTX target) */
    int cc_major = 0, cc_minor = 0;
    cu_cuDeviceGetAttribute(&cc_major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, g_device);
    cu_cuDeviceGetAttribute(&cc_minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, g_device);
    if (cc_major < 5) {
        fprintf(stderr, "CUDA: GPU compute capability %d.%d too old (need 5.0+)\n",
                cc_major, cc_minor);
        return -2;
    }

    /* Create CUDA context */
    res = cu_cuCtxCreate(&g_context, 0, g_device);
    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDA: Failed to create context (error %d)\n", res);
        return -1;
    }

    /* JIT-compile PTX to native GPU code */
    char jit_error_log[4096] = {0};
    char jit_info_log[4096] = {0};

    CUjit_option jit_options[] = {
        CU_JIT_ERROR_LOG_BUFFER,
        CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES,
        CU_JIT_INFO_LOG_BUFFER,
        CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES,
    };
    void* jit_option_values[] = {
        (void*)jit_error_log,
        (void*)(size_t)sizeof(jit_error_log),
        (void*)jit_info_log,
        (void*)(size_t)sizeof(jit_info_log),
    };

    res = cu_cuModuleLoadDataEx(&g_module, ptx_source, 4, jit_options, jit_option_values);
    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDA: Failed to JIT-compile PTX (error %d)\n", res);
        if (jit_error_log[0]) {
            fprintf(stderr, "CUDA JIT error:\n%s\n", jit_error_log);
        }
        cu_cuCtxDestroy(g_context);
        g_context = nullptr;
        return -1;
    }

    /* Get kernel function handles */
    res = cu_cuModuleGetFunction(&g_kernel_320, g_module, "fermat_kernel_320");
    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDA: Failed to find kernel 'fermat_kernel_320' (error %d)\n", res);
        cu_cuModuleUnload(g_module);
        cu_cuCtxDestroy(g_context);
        g_module = nullptr;
        g_context = nullptr;
        return -1;
    }

    res = cu_cuModuleGetFunction(&g_kernel_352, g_module, "fermat_kernel_352");
    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDA: Failed to find kernel 'fermat_kernel_352' (error %d)\n", res);
        cu_cuModuleUnload(g_module);
        cu_cuCtxDestroy(g_context);
        g_module = nullptr;
        g_context = nullptr;
        return -1;
    }

    /* Load self-test kernel (optional — failure is non-fatal) */
    res = cu_cuModuleGetFunction(&g_kernel_selftest, g_module, "fermat_selftest");
    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDA: Warning: self-test kernel not found (error %d)\n", res);
        g_kernel_selftest = nullptr;
    }

    g_initialized = 1;

    /* Run self-test to verify Montgomery math */
    if (g_kernel_selftest) {
        int st = cuda_fermat_selftest();
        if (st != 0) {
            fprintf(stderr, "CUDA: SELF-TEST FAILED — GPU Fermat math is broken!\n");
            /* Don't return error — log it prominently but let mining proceed
             * so the user can see the self-test output */
        }
    }

    return 0;
}

void cuda_fermat_cleanup(void) {
    if (!g_initialized) return;

    if (g_module) {
        cu_cuModuleUnload(g_module);
        g_module = nullptr;
    }
    if (g_context) {
        cu_cuCtxDestroy(g_context);
        g_context = nullptr;
    }

    g_kernel_320 = nullptr;
    g_kernel_352 = nullptr;
    g_kernel_selftest = nullptr;
    g_initialized = 0;
}

int cuda_fermat_batch(uint8_t *h_results, const uint32_t *h_primes,
                      uint32_t count, int bits) {
    if (!g_initialized) return -1;
    if (count == 0) return 0;

    CUresult res;
    int limbs = (bits <= 320) ? 10 : 11;
    size_t primes_size = (size_t)count * limbs * sizeof(uint32_t);
    size_t results_size = (size_t)count * sizeof(uint8_t);

    /* Allocate device memory */
    CUdeviceptr d_primes = 0, d_results = 0;

    res = cu_cuMemAlloc(&d_primes, primes_size);
    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDA: cuMemAlloc primes failed (error %d)\n", res);
        return -1;
    }

    res = cu_cuMemAlloc(&d_results, results_size);
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_primes);
        fprintf(stderr, "CUDA: cuMemAlloc results failed (error %d)\n", res);
        return -1;
    }

    /* Copy primes to device */
    res = cu_cuMemcpyHtoD(d_primes, h_primes, primes_size);
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        fprintf(stderr, "CUDA: cuMemcpyHtoD failed (error %d)\n", res);
        return -1;
    }

    /* Select kernel */
    CUfunction kernel = (bits <= 320) ? g_kernel_320 : g_kernel_352;

    /* Launch kernel */
    unsigned int grid_x = (count + BLOCK_SIZE - 1) / BLOCK_SIZE;

    void* kernel_params[] = {
        &d_results,
        &d_primes,
        &count
    };

    res = cu_cuLaunchKernel(kernel,
                            grid_x, 1, 1,          /* grid dimensions */
                            BLOCK_SIZE, 1, 1,       /* block dimensions */
                            0,                      /* shared memory */
                            nullptr,                /* stream (default) */
                            kernel_params,          /* kernel parameters */
                            nullptr);               /* extra */
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        fprintf(stderr, "CUDA: cuLaunchKernel failed (error %d)\n", res);
        return -1;
    }

    /* Synchronize and copy results back */
    res = cu_cuCtxSynchronize();
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        fprintf(stderr, "CUDA: cuCtxSynchronize failed (error %d)\n", res);
        return -1;
    }

    res = cu_cuMemcpyDtoH(h_results, d_results, results_size);
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        cu_cuMemFree(d_primes);
        fprintf(stderr, "CUDA: cuMemcpyDtoH failed (error %d)\n", res);
        return -1;
    }

    /* Free device memory */
    cu_cuMemFree(d_results);
    cu_cuMemFree(d_primes);

    return 0;
}

int cuda_fermat_selftest(void) {
    if (!g_initialized || !g_kernel_selftest) return -1;

    CUresult res;

    /* Allocate 4 bytes on device for results:
     * [0] = fermat320(secp256k1_prime)  — expect 1
     * [1] = fermat320(mersenne_127)     — expect 1
     * [2] = fermat320(15)               — expect 0
     * [3] = sentinel 0xAA               — expect 0xAA */
    CUdeviceptr d_results = 0;
    res = cu_cuMemAlloc(&d_results, 4);
    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDA selftest: cuMemAlloc failed (error %d)\n", res);
        return -1;
    }

    /* Zero out results */
    uint8_t zeros[4] = {0, 0, 0, 0};
    cu_cuMemcpyHtoD(d_results, zeros, 4);

    /* Launch self-test kernel: 1 block, 1 thread */
    void* params[] = { &d_results };
    res = cu_cuLaunchKernel(g_kernel_selftest,
                            1, 1, 1,    /* grid */
                            1, 1, 1,    /* block (only thread 0 runs) */
                            0, nullptr, params, nullptr);
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        fprintf(stderr, "CUDA selftest: launch failed (error %d)\n", res);
        return -1;
    }

    res = cu_cuCtxSynchronize();
    if (res != CUDA_SUCCESS) {
        cu_cuMemFree(d_results);
        fprintf(stderr, "CUDA selftest: sync failed (error %d)\n", res);
        return -1;
    }

    uint8_t h_results[4] = {0};
    res = cu_cuMemcpyDtoH(h_results, d_results, 4);
    cu_cuMemFree(d_results);

    if (res != CUDA_SUCCESS) {
        fprintf(stderr, "CUDA selftest: readback failed (error %d)\n", res);
        return -1;
    }

    fprintf(stderr, "CUDA selftest results:\n");
    fprintf(stderr, "  secp256k1 prime (expect 1): %d\n", h_results[0]);
    fprintf(stderr, "  Mersenne M127   (expect 1): %d\n", h_results[1]);
    fprintf(stderr, "  Composite 15    (expect 0): %d\n", h_results[2]);
    fprintf(stderr, "  Sentinel        (expect AA): %02X\n", h_results[3]);

    int pass = 1;
    if (h_results[0] != 1) { fprintf(stderr, "  FAIL: secp256k1 prime returned %d\n", h_results[0]); pass = 0; }
    if (h_results[1] != 1) { fprintf(stderr, "  FAIL: Mersenne M127 returned %d\n", h_results[1]); pass = 0; }
    if (h_results[2] != 0) { fprintf(stderr, "  FAIL: Composite 15 returned %d\n", h_results[2]); pass = 0; }
    if (h_results[3] != 0xAA) { fprintf(stderr, "  FAIL: Sentinel not 0xAA (kernel didn't run?)\n"); pass = 0; }

    if (pass) {
        fprintf(stderr, "CUDA selftest: PASSED — Montgomery math verified\n");
        return 0;
    } else {
        fprintf(stderr, "CUDA selftest: FAILED\n");
        return -1;
    }
}

int cuda_get_device_count(void) {
    if (!cuda_is_loaded()) {
        if (cuda_load() != 0) return 0;
    }

    int count = 0;
    if (cu_cuDeviceGetCount(&count) != CUDA_SUCCESS) {
        return 0;
    }
    return count;
}

const char* cuda_get_device_name(int device_id) {
    static char name[256];
    if (!cuda_is_loaded()) {
        if (cuda_load() != 0) return "N/A";
    }

    CUdevice dev;
    if (cu_cuDeviceGet(&dev, device_id) != CUDA_SUCCESS) {
        return "Unknown";
    }

    if (cu_cuDeviceGetName(name, sizeof(name), dev) != CUDA_SUCCESS) {
        return "Unknown";
    }

    return name;
}

size_t cuda_get_device_memory(int device_id) {
    if (!cuda_is_loaded()) {
        if (cuda_load() != 0) return 0;
    }

    CUdevice dev;
    size_t memory = 0;
    if (cu_cuDeviceGet(&dev, device_id) != CUDA_SUCCESS) {
        return 0;
    }

    if (cu_cuDeviceTotalMem(&memory, dev) != CUDA_SUCCESS) {
        return 0;
    }

    return memory;
}

int cuda_get_sm_count(int device_id) {
    if (!cuda_is_loaded()) {
        if (cuda_load() != 0) return 0;
    }

    CUdevice dev;
    int sm_count = 0;
    if (cu_cuDeviceGet(&dev, device_id) != CUDA_SUCCESS) {
        return 0;
    }

    if (cu_cuDeviceGetAttribute(&sm_count, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, dev) != CUDA_SUCCESS) {
        return 0;
    }

    return sm_count;
}

int cuda_is_available(void) {
    return cuda_get_device_count() > 0 ? 1 : 0;
}
