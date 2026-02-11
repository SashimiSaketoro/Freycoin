// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * OpenCL Fermat Primality Test Implementation
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 *
 * Uses dynamic OpenCL loading -- no SDK required at build time.
 * OpenCL.dll / libOpenCL.so is loaded at runtime if available.
 *
 * Optimizations:
 * - Persistent buffer reuse across batches (avoid per-batch alloc/free)
 * - CL_MEM_USE_HOST_PTR on unified memory platforms (zero-copy)
 */

#include "opencl_fermat.h"
#include "opencl_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Global OpenCL state */
static cl_platform_id g_platform = nullptr;
static cl_device_id g_device = nullptr;
static cl_context g_context = nullptr;
static cl_command_queue g_queue = nullptr;
static cl_program g_program = nullptr;
static cl_kernel g_kernel_320 = nullptr;
static cl_kernel g_kernel_352 = nullptr;
static int g_initialized = 0;
static char g_device_name[256] = {0};
static size_t g_device_memory = 0;

/* Unified memory detection */
static int g_unified_memory = 0;

/* Persistent buffer state */
static cl_mem g_primes_buf = nullptr;
static cl_mem g_results_buf = nullptr;
static size_t g_primes_buf_size = 0;
static size_t g_results_buf_size = 0;

/* Embedded kernel source (generated from fermat.cl) */
static const char* kernel_source =
#include "fermat_cl_source.h"
;

/* Alternative: Load kernel from file at runtime */
static char* load_kernel_file(const char* filename, size_t* size) {
    FILE* f = fopen(filename, "rb");
    if (!f) return nullptr;

    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* source = (char*)malloc(*size + 1);
    if (!source) {
        fclose(f);
        return nullptr;
    }

    size_t nread = fread(source, 1, *size, f);
    fclose(f);
    if (nread != (size_t)*size) {
        /* Short read: kernel source would be truncated/uninitialized */
        free(source);
        return nullptr;
    }
    source[*size] = '\0';

    return source;
}

/* Release persistent buffers */
static void release_persistent_buffers(void) {
    if (g_primes_buf) {
        ocl_clReleaseMemObject(g_primes_buf);
        g_primes_buf = nullptr;
        g_primes_buf_size = 0;
    }
    if (g_results_buf) {
        ocl_clReleaseMemObject(g_results_buf);
        g_results_buf = nullptr;
        g_results_buf_size = 0;
    }
}

/* Ensure persistent buffers are large enough for the given batch */
static int ensure_buffers(size_t primes_size, size_t results_size) {
    cl_int err;

    if (g_primes_buf && g_primes_buf_size >= primes_size &&
        g_results_buf && g_results_buf_size >= results_size) {
        return 0;  /* Existing buffers are large enough */
    }

    /* Release old buffers */
    release_persistent_buffers();

    /* Allocate with 25% headroom to reduce reallocations */
    size_t alloc_primes = primes_size + (primes_size / 4);
    size_t alloc_results = results_size + (results_size / 4);
    if (alloc_primes < 16384) alloc_primes = 16384;
    if (alloc_results < 4096) alloc_results = 4096;

    if (g_unified_memory) {
        /* Unified memory: use CL_MEM_ALLOC_HOST_PTR for optimal zero-copy.
         * We'll use clEnqueueWriteBuffer to update contents, but the driver
         * can map host memory directly without PCIe transfer. */
        g_primes_buf = ocl_clCreateBuffer(g_context,
                                           CL_MEM_READ_ONLY | CL_MEM_ALLOC_HOST_PTR,
                                           alloc_primes, nullptr, &err);
    } else {
        g_primes_buf = ocl_clCreateBuffer(g_context, CL_MEM_READ_ONLY,
                                           alloc_primes, nullptr, &err);
    }
    if (err != CL_SUCCESS) return -1;

    if (g_unified_memory) {
        g_results_buf = ocl_clCreateBuffer(g_context,
                                            CL_MEM_WRITE_ONLY | CL_MEM_ALLOC_HOST_PTR,
                                            alloc_results, nullptr, &err);
    } else {
        g_results_buf = ocl_clCreateBuffer(g_context, CL_MEM_WRITE_ONLY,
                                            alloc_results, nullptr, &err);
    }
    if (err != CL_SUCCESS) {
        ocl_clReleaseMemObject(g_primes_buf);
        g_primes_buf = nullptr;
        return -1;
    }

    g_primes_buf_size = alloc_primes;
    g_results_buf_size = alloc_results;
    return 0;
}

int opencl_fermat_init(int device_id) {
    cl_int err;
    cl_uint num_platforms, num_devices;

    if (g_initialized) return 0;

    /* Ensure OpenCL is dynamically loaded */
    if (!opencl_is_loaded()) {
        if (opencl_load() != 0) {
            return -2;  /* OpenCL library not available */
        }
    }

    /* Get platform */
    err = ocl_clGetPlatformIDs(1, &g_platform, &num_platforms);
    if (err != CL_SUCCESS || num_platforms == 0) {
        return -2;
    }

    /* Get device */
    err = ocl_clGetDeviceIDs(g_platform, CL_DEVICE_TYPE_GPU, 0, nullptr, &num_devices);
    if (err != CL_SUCCESS || num_devices == 0) {
        return -2;
    }

    cl_device_id* devices = (cl_device_id*)malloc(sizeof(cl_device_id) * num_devices);
    err = ocl_clGetDeviceIDs(g_platform, CL_DEVICE_TYPE_GPU, num_devices, devices, nullptr);
    if (err != CL_SUCCESS) {
        free(devices);
        return -1;
    }

    if ((cl_uint)device_id >= num_devices) {
        device_id = 0;
    }
    g_device = devices[device_id];
    free(devices);

    /* Get device info */
    ocl_clGetDeviceInfo(g_device, CL_DEVICE_NAME, sizeof(g_device_name), g_device_name, nullptr);
    ocl_clGetDeviceInfo(g_device, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(g_device_memory), &g_device_memory, nullptr);

    /* Detect unified memory (Apple Silicon, integrated GPUs, etc.) */
    cl_bool unified = CL_FALSE;
    err = ocl_clGetDeviceInfo(g_device, CL_DEVICE_HOST_UNIFIED_MEMORY,
                               sizeof(unified), &unified, nullptr);
    g_unified_memory = (err == CL_SUCCESS && unified == CL_TRUE) ? 1 : 0;

    /* Create context */
    g_context = ocl_clCreateContext(nullptr, 1, &g_device, nullptr, nullptr, &err);
    if (err != CL_SUCCESS) {
        return -1;
    }

    /* Create command queue */
    g_queue = ocl_clCreateCommandQueue(g_context, g_device, 0, &err);
    if (err != CL_SUCCESS) {
        ocl_clReleaseContext(g_context);
        return -1;
    }

    /* Try to load kernel from file first, fall back to embedded */
    size_t kernel_size;
    char* source = load_kernel_file("fermat.cl", &kernel_size);
    if (!source) {
        source = (char*)kernel_source;
        kernel_size = strlen(kernel_source);
    }

    /* Create program */
    g_program = ocl_clCreateProgramWithSource(g_context, 1, (const char**)&source, &kernel_size, &err);
    if (source != kernel_source) {
        free(source);
    }
    if (err != CL_SUCCESS) {
        ocl_clReleaseCommandQueue(g_queue);
        ocl_clReleaseContext(g_context);
        return -1;
    }

    /* Build program */
    err = ocl_clBuildProgram(g_program, 1, &g_device, "", nullptr, nullptr);
    if (err != CL_SUCCESS) {
        size_t log_size;
        ocl_clGetProgramBuildInfo(g_program, g_device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &log_size);
        char* log = (char*)malloc(log_size + 1);
        ocl_clGetProgramBuildInfo(g_program, g_device, CL_PROGRAM_BUILD_LOG, log_size, log, nullptr);
        fprintf(stderr, "OpenCL build error:\n%s\n", log);
        free(log);

        ocl_clReleaseProgram(g_program);
        ocl_clReleaseCommandQueue(g_queue);
        ocl_clReleaseContext(g_context);
        return -1;
    }

    /* Create kernels */
    g_kernel_320 = ocl_clCreateKernel(g_program, "fermat_kernel_320", &err);
    if (err != CL_SUCCESS) {
        ocl_clReleaseProgram(g_program);
        ocl_clReleaseCommandQueue(g_queue);
        ocl_clReleaseContext(g_context);
        return -1;
    }

    g_kernel_352 = ocl_clCreateKernel(g_program, "fermat_kernel_352", &err);
    if (err != CL_SUCCESS) {
        ocl_clReleaseKernel(g_kernel_320);
        ocl_clReleaseProgram(g_program);
        ocl_clReleaseCommandQueue(g_queue);
        ocl_clReleaseContext(g_context);
        return -1;
    }

    g_initialized = 1;
    return 0;
}

void opencl_fermat_cleanup(void) {
    if (!g_initialized) return;

    /* Release persistent buffers */
    release_persistent_buffers();

    if (g_kernel_320) ocl_clReleaseKernel(g_kernel_320);
    if (g_kernel_352) ocl_clReleaseKernel(g_kernel_352);
    if (g_program) ocl_clReleaseProgram(g_program);
    if (g_queue) ocl_clReleaseCommandQueue(g_queue);
    if (g_context) ocl_clReleaseContext(g_context);

    g_kernel_320 = nullptr;
    g_kernel_352 = nullptr;
    g_program = nullptr;
    g_queue = nullptr;
    g_context = nullptr;
    g_initialized = 0;
    g_unified_memory = 0;
}

int opencl_fermat_batch(uint8_t *h_results, const uint32_t *h_primes,
                        uint32_t count, int bits) {
    if (!g_initialized) return -1;
    if (count == 0) return 0;

    cl_int err;
    int limbs = (bits <= 320) ? 10 : 11;
    size_t primes_size = (size_t)count * limbs * sizeof(uint32_t);
    size_t results_size = (size_t)count * sizeof(uint8_t);

    /* Ensure persistent results buffer is large enough */
    if (ensure_buffers(primes_size, results_size) != 0) {
        return -1;
    }

    /* Write primes data into the persistent g_primes_buf.
     * SECURITY: Using the persistent driver-allocated buffer instead of
     * per-batch CL_MEM_USE_HOST_PTR avoids:
     *   (a) alignment violations (std::vector data may not meet CL_DEVICE_MEM_BASE_ADDR_ALIGN)
     *   (b) per-batch buffer create/destroy overhead
     *   (c) the wasted g_primes_buf allocation that was never used before */
    err = ocl_clEnqueueWriteBuffer(g_queue, g_primes_buf, CL_TRUE, 0,
                                    primes_size, h_primes, 0, nullptr, nullptr);
    if (err != CL_SUCCESS) return -1;

    /* Select kernel */
    cl_kernel kernel = (bits <= 320) ? g_kernel_320 : g_kernel_352;

    /* Set kernel arguments */
    ocl_clSetKernelArg(kernel, 0, sizeof(cl_mem), &g_results_buf);
    ocl_clSetKernelArg(kernel, 1, sizeof(cl_mem), &g_primes_buf);
    ocl_clSetKernelArg(kernel, 2, sizeof(uint32_t), &count);

    /* Execute kernel */
    size_t global_size = ((count + 63) / 64) * 64;
    size_t local_size = 64;
    err = ocl_clEnqueueNDRangeKernel(g_queue, kernel, 1, nullptr, &global_size, &local_size, 0, nullptr, nullptr);
    if (err != CL_SUCCESS) return -1;

    /* Read results from the persistent results buffer */
    err = ocl_clEnqueueReadBuffer(g_queue, g_results_buf, CL_TRUE, 0, results_size, h_results, 0, nullptr, nullptr);
    if (err != CL_SUCCESS) return -1;

    return 0;
}

int opencl_get_device_count(void) {
    if (!opencl_is_loaded()) {
        if (opencl_load() != 0) return 0;
    }

    cl_platform_id platform;
    cl_uint num_platforms, num_devices;

    if (ocl_clGetPlatformIDs(1, &platform, &num_platforms) != CL_SUCCESS || num_platforms == 0) {
        return 0;
    }

    if (ocl_clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 0, nullptr, &num_devices) != CL_SUCCESS) {
        return 0;
    }

    return (int)num_devices;
}

const char* opencl_get_device_name(int device_id) {
    static char name[256];
    if (!opencl_is_loaded()) {
        if (opencl_load() != 0) return "N/A";
    }

    cl_platform_id platform;
    cl_uint num_platforms, num_devices;

    if (ocl_clGetPlatformIDs(1, &platform, &num_platforms) != CL_SUCCESS) {
        return "Unknown";
    }

    cl_device_id* devices = (cl_device_id*)malloc(sizeof(cl_device_id) * 16);
    if (ocl_clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 16, devices, &num_devices) != CL_SUCCESS) {
        free(devices);
        return "Unknown";
    }

    if ((cl_uint)device_id >= num_devices) {
        device_id = 0;
    }

    ocl_clGetDeviceInfo(devices[device_id], CL_DEVICE_NAME, sizeof(name), name, nullptr);
    free(devices);

    return name;
}

size_t opencl_get_device_memory(int device_id) {
    if (!opencl_is_loaded()) {
        if (opencl_load() != 0) return 0;
    }

    cl_platform_id platform;
    cl_uint num_platforms, num_devices;
    size_t memory = 0;

    if (ocl_clGetPlatformIDs(1, &platform, &num_platforms) != CL_SUCCESS) {
        return 0;
    }

    cl_device_id* devices = (cl_device_id*)malloc(sizeof(cl_device_id) * 16);
    if (ocl_clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 16, devices, &num_devices) != CL_SUCCESS) {
        free(devices);
        return 0;
    }

    if ((cl_uint)device_id >= num_devices) {
        device_id = 0;
    }

    ocl_clGetDeviceInfo(devices[device_id], CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(memory), &memory, nullptr);
    free(devices);

    return memory;
}

int opencl_is_available(void) {
    return opencl_get_device_count() > 0 ? 1 : 0;
}
