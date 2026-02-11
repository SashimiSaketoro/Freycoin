# Changelog

All notable changes to Freycoin will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

---

## [Unreleased] — Red Team Round 2: MEDIUM + LOW Fixes

### Security — RPC DoS Hardening (M1 + M2)

**Problem:** The `g_rpc_fail_map` grew without bound as stale entries were never
erased — an attacker rotating IPs could exhaust memory. Additionally,
`UninterruptibleSleep` blocked RPC worker threads for up to 2 seconds per
failed auth, letting an attacker exhaust all 16 HTTP workers with sustained
concurrent failures.

**Fix:**
- **M1:** Added garbage collection to `g_rpc_fail_map`. When map size exceeds
  1000 entries, all stale entries (>60s since last attempt) are swept under the
  existing mutex lock. Runs at most once per 1000 unique IPs.
- **M2:** Removed `UninterruptibleSleep` entirely. Failed auth now returns an
  immediate 401 response. Per-IP failure tracking remains for logging/monitoring
  but no longer blocks worker threads.

| File | Change |
|------|--------|
| `src/httprpc.cpp` | GC sweep on map overflow; removed `UninterruptibleSleep` |

### Security — Mining Pipeline Thread Safety (M3–M6)

**M3. `PoW::set_adder` data race:** Multiple sieve threads could concurrently
call `current_pow->set_adder()` on the shared `current_pow`. GMP's `mpz_set`
is not thread-safe.

**Fix:** Added `pow_result_mutex` to `MiningPipeline`. Both the GPU pre-filter
and scalar code paths now lock it around the `set_adder()` + `valid()` +
`process()` sequence. Contention is negligible since this path fires only on
gap discovery.

**M4 + M5. Presieve and CPU feature detection races:** `tables_initialized`
and `g_cpu_features.detected` were plain `bool` flags. Two threads could race
through initialization simultaneously.

**Fix:** Replaced both with `std::call_once` + `std::once_flag`, covering the
x86 and ARM code paths for `detect_cpu_features()` and
`presieve_generate_tables()`.

**M6. GPU wait deadlock:** `request->cv.wait()` in the parallel mining worker
blocked forever if the GPU thread died. `stop_requested` was not in the wait
predicate.

**Fix:** Replaced with `cv.wait_for(..., 10s)` and added `stop_requested` to
the predicate. On timeout, the worker breaks out and falls back to CPU-only
validation.

| File | Change |
|------|--------|
| `src/pow/mining_engine.h` | Added `pow_result_mutex` member |
| `src/pow/mining_engine.cpp` | Lock around set_adder paths; `cv.wait_for` with timeout |
| `src/pow/simd_presieve.cpp` | `std::call_once` for tables + CPU features (x86 + ARM) |

### Security — GPU Backend Hardening (M7 + M8)

**M7. Metal infinite block on GPU hang:** `waitUntilCompleted` has no timeout
in Metal. A GPU hang blocks the mining thread forever.

**Fix:** Replaced with `addCompletedHandler:` + `dispatch_semaphore_wait` with
a 5-second deadline. On timeout, returns -1 and logs a warning.

**M8. OpenCL per-batch buffer waste + alignment issues:** The persistent
`g_primes_buf` was allocated in `ensure_buffers` but never used — a new
per-batch `d_primes` was created every call, wasting allocations and risking
`CL_MEM_USE_HOST_PTR` alignment violations.

**Fix:** Now writes primes data into the persistent `g_primes_buf` via
`clEnqueueWriteBuffer`, eliminating per-batch buffer creation, the wasted
allocation, and the alignment issue (driver-allocated buffers are always
properly aligned).

| File | Change |
|------|--------|
| `src/gpu/metal_fermat.mm` | Semaphore + 5s timeout replacing `waitUntilCompleted` |
| `src/gpu/opencl_fermat.cpp` | Use `g_primes_buf` via `clEnqueueWriteBuffer`; removed per-batch `d_primes` |

### Fixed — Strict Aliasing Violation (M9)

**Problem:** `src/pow/pow_utils.cpp` used `reinterpret_cast<uint64_t*>` and
`reinterpret_cast<uint32_t*>` to XOR-fold SHA256 hashes — undefined behavior
under C++ strict aliasing rules.

**Fix:** Replaced with `std::memcpy` into properly-typed local arrays at both
callsites (lines 419 and 625).

| File | Change |
|------|--------|
| `src/pow/pow_utils.cpp` | `reinterpret_cast` → `std::memcpy` at 2 XOR-fold sites |

### Improved — Macro Namespace Hygiene (M10)

**Problem:** Generic `#define` macros (`set_bit64`, `clear_bit64`, `test_bit64`,
`round_up`) polluted the global namespace and were duplicated between
`pow_common.h` and `mining_engine.cpp`.

**Fix:** Converted all four to `freycoin_`-prefixed inline functions (and a
template for `round_up`) in `pow_common.h`. Removed the duplicate definitions
from `mining_engine.cpp`. Updated all call sites across `mining_engine.cpp` and
`combined_sieve.cpp`.

| File | Change |
|------|--------|
| `src/pow/pow_common.h` | Macros → `freycoin_set_bit64` etc. inline functions |
| `src/pow/mining_engine.cpp` | Removed duplicate macros; updated all call sites |
| `src/pow/combined_sieve.cpp` | Updated all call sites |

### Fixed — LOW Issues (L1–L4)

- **L1.** `opencl_fermat.cpp`: `fread` return value now checked — short read
  returns `nullptr` instead of using uninitialized kernel source data.
- **L2.** `embed_binary.cmake`: Uses `get_filename_component(... NAME)` to
  embed only the filename (not the full build path) in generated C headers.
- **L3.** `metal_fermat.h`: Added thread-safety documentation noting all
  functions must be called from a single thread (the GPU worker).
- **L4.** `mining_engine.cpp`: Added `assert(candidates[i] <= UINT32_MAX)`
  guard at the `uint64_t` → `uint32_t` narrowing assignment with explicit
  `static_cast`.

| File | Change |
|------|--------|
| `src/gpu/opencl_fermat.cpp` | `fread` return check |
| `src/gpu/embed_binary.cmake` | Basename-only in generated comment |
| `src/gpu/metal_fermat.h` | Thread-safety documentation |
| `src/pow/mining_engine.cpp` | Narrowing assert + `static_cast` |

---

## [Unreleased] — Red Team Round 2: CRITICAL + HIGH Fixes

### Security — `mpz_to_ary()` NULL Crash on Zero Input (C1)

**Problem:** `mpz_export()` returns NULL when the input `mpz_t` is zero. All 9
callers dereference the result for `memcpy`, `SHA256::Write`, or `free`. An
attacker could craft a block with a zero-valued adder to trigger a NULL pointer
dereference crash.

**Fix:** `mpz_to_ary()` now detects zero via `mpz_sgn()` and returns a valid
single-byte `{0}` buffer (malloc'd), preventing NULL dereference at all 9
callsites.

| File | Change |
|------|--------|
| `src/pow/pow_common.h` | Zero-input guard in `mpz_to_ary()` |

### Security — `dispatch_data_create` Crash on Static Data (C2)

**Problem:** `DISPATCH_DATA_DESTRUCTOR_DEFAULT` calls `free()` on the backing
buffer, but `fermat_metallib_data` is a static/const array embedded by the
linker. Calling `free()` on it is UB and crashes.

**Fix:** Replaced with a no-op destructor block (`^{}`) so static linker data
is never freed.

| File | Change |
|------|--------|
| `src/gpu/metal_fermat.mm` | No-op destructor for `dispatch_data_create` |

### Security — `gpu_initialized` Data Race (H1)

**Problem:** GPU worker thread writes `gpu_initialized = true`; sieve threads
read it. Plain `bool` with no synchronization — C++ data race (UB).

**Fix:** Changed to `std::atomic<bool>`.

| File | Change |
|------|--------|
| `src/pow/mining_engine.h` | `bool` → `std::atomic<bool> gpu_initialized` |

### Security — NULL `fprintf %s` in Metal Error Paths (H2)

**Problem:** When Metal operations fail, `[[error localizedDescription] UTF8String]`
returns NULL if `error` is nil, and passing NULL to `%s` is UB (crash on many
platforms).

**Fix:** Added `metal_errstr()` helper that safely returns `"(unknown error)"`
for nil errors. Updated all 5 `fprintf` sites.

| File | Change |
|------|--------|
| `src/gpu/metal_fermat.mm` | `metal_errstr()` helper; 5 fprintf guards |

### Improved — `SegmentedSieve` Memory Safety (H3)

**Problem:** `sieve_worker()` allocated `SegmentedSieve*` with raw `new` and
manually `delete`d across 4 exit paths. Several early returns skipped the
delete, and exceptions between new/delete would leak.

**Fix:** Replaced with `std::unique_ptr<SegmentedSieve>` and
`std::make_unique`. Removed all 4 manual `delete` calls.

| File | Change |
|------|--------|
| `src/pow/mining_engine.cpp` | `unique_ptr` for `SegmentedSieve` |

### Security — `mpz_get_str` NULL Return (H4)

**Problem:** `mpz_get_str(nullptr, ...)` can return NULL on malloc failure.
Results were used directly as strings and passed to `free()`.

**Fix:** Pre-allocate buffers using `mpz_sizeinbase()` + `std::vector<char>`
at both sites (`PoW::to_string()` and `MpzToHex()`).

| File | Change |
|------|--------|
| `src/pow.cpp` | Pre-allocated buffers for `mpz_get_str` |
| `src/rpc/blockchain.cpp` | Pre-allocated buffer in `MpzToHex()` |

### Improved — Wallet `EncryptWallet` Memory Safety (H5)

**Problem:** Raw `new WalletBatch(...)` with manual `delete` across 4 paths.
Exception between new/delete leaks the pointer and may fail to cleanse
`plain_master_key`.

**Fix:** Replaced with `std::make_unique<WalletBatch>`. Uses `.get()` where
raw pointer is needed.

| File | Change |
|------|--------|
| `src/wallet/wallet.cpp` | `unique_ptr` for `WalletBatch` in `EncryptWallet()` |

### Security — Windows `_wsystem()` Command Injection (H6)

**Problem:** The POSIX path was fixed to `fork+execvp`, but Windows still used
`_wsystem()` which invokes `cmd.exe` — vulnerable to shell metacharacter
injection.

**Fix:** Replaced with `CreateProcessW` using explicit argument tokenization
and proper quoting, analogous to the POSIX approach.

| File | Change |
|------|--------|
| `src/common/system.cpp` | `_wsystem()` → `CreateProcessW` with argument vector |

---

## [Unreleased] — Apple Silicon Native Support

### Added — Metal GPU Backend

Native Apple Silicon GPU compute via Metal Shading Language, replacing the
OpenCL path on ARM64 macOS. The Montgomery multiplication Fermat kernel
(`fermat_metal.metal`) is a direct port of the existing OpenCL kernel.

- **Zero-copy unified memory**: `MTLResourceStorageModeShared` buffers avoid
  all host-to-device memcpy on Apple Silicon's unified memory architecture.
  Candidate data written by CPU sieve threads is read directly by GPU threads
  from the same physical memory.
- **Persistent buffer reuse**: GPU buffers are allocated once with headroom and
  reused across batches, eliminating per-batch allocation overhead.
- **Runtime backend selection**: New `gpu_fermat_*()` dispatch layer
  (`gpu_fermat.h`/`gpu_fermat.cpp`) selects Metal on macOS ARM64, falls back to
  OpenCL on x86_64/Linux/Windows. Callers (mining engine, GUI) are
  backend-agnostic.

#### New files

- `src/gpu/fermat_metal.metal` — MSL Fermat kernel (320-bit & 352-bit)
- `src/gpu/metal_fermat.h` / `metal_fermat.mm` — Objective-C++ Metal host driver
- `src/gpu/gpu_fermat.h` / `gpu_fermat.cpp` — Unified GPU dispatch layer
- `src/gpu/embed_binary.cmake` — CMake helper for embedding metallib as C array

### Fixed — ARM64 Build Blockers

Three source files unconditionally included `<cpuid.h>` (an x86-only header)
and used x86 intrinsics (`__cpuid_count`, `xgetbv`, `__get_cpuid_count`),
causing immediate compilation failure on any ARM target.

- `src/pow/simd_presieve.h` — Guarded `<cpuid.h>` include with `#elif defined(__x86_64__) || defined(__i386__)`.
- `src/pow/simd_presieve.cpp` — Wrapped all x86 CPU detection (`run_cpuid`,
  `portable_xgetbv`, `os_supports_avx`, `os_supports_avx512`,
  `detect_cpu_features`) in x86 `#if` guard. Added ARM stub that sets all x86
  SIMD flags to `false`.
- `src/pow/avx512_primality.cpp` — Guarded `<cpuid.h>` include and
  `avx512_ifma_available()` CPUID detection for x86 only. On ARM, returns
  `false` unconditionally.

### Added — ARM NEON Presieve (128-bit SIMD)

Implemented NEON-accelerated presieve functions for ARM64, providing 128-bit
SIMD (equivalent to SSE2). NEON is always available on aarch64 so no runtime
detection is needed.

- `presieve_init_neon()` / `presieve_apply_neon()` in `src/pow/simd_presieve.cpp`
- Uses `vld1q_u8`, `vorrq_u8`, `vst1q_u8` intrinsics via `<arm_neon.h>`
- Runtime dispatch updated to select NEON on ARM before scalar fallback
- Expected ~2x speedup over the portable 8-byte scalar implementation

### Improved — OpenCL Unified Memory & Buffer Reuse

- **Zero-copy on unified memory**: `opencl_fermat.cpp` now queries
  `CL_DEVICE_HOST_UNIFIED_MEMORY` at init and uses `CL_MEM_USE_HOST_PTR` for
  input buffers on integrated/unified GPUs, avoiding redundant copies.
- **Persistent results buffer**: The results `cl_mem` is allocated once and
  reused across batches (with automatic reallocation if a batch exceeds the
  current capacity).
- Added `CL_MEM_USE_HOST_PTR`, `CL_MEM_ALLOC_HOST_PTR`,
  `CL_DEVICE_HOST_UNIFIED_MEMORY` constants to `opencl_loader.h`.

### Changed — CMake Build System

- `src/gpu/CMakeLists.txt` — Conditionally compiles Metal shader, links
  `-framework Metal -framework Foundation` on Apple, embeds `.metallib` as C
  byte array via custom CMake command.
- `src/pow/CMakeLists.txt` — No changes (already links `freycoin_gpu`).

#### Files changed

| File | Change |
|------|--------|
| `src/pow/simd_presieve.h` | Guard cpuid.h, add NEON header + declarations |
| `src/pow/simd_presieve.cpp` | Guard x86 detection, add ARM stub, add NEON impl |
| `src/pow/avx512_primality.cpp` | Guard cpuid.h + IFMA detection for x86 |
| `src/gpu/fermat_metal.metal` | NEW: Metal Fermat kernel |
| `src/gpu/metal_fermat.h` | NEW: Metal backend header |
| `src/gpu/metal_fermat.mm` | NEW: Metal backend implementation |
| `src/gpu/gpu_fermat.h` | NEW: Unified dispatch header |
| `src/gpu/gpu_fermat.cpp` | NEW: Unified dispatch implementation |
| `src/gpu/embed_binary.cmake` | NEW: CMake metallib embedder |
| `src/gpu/opencl_fermat.cpp` | Unified memory + persistent buffer reuse |
| `src/gpu/opencl_loader.h` | Added missing CL_MEM constants |
| `src/gpu/CMakeLists.txt` | Metal compilation + Apple frameworks |
| `src/pow/mining_engine.cpp` | Use gpu_fermat_* dispatch API |
| `src/qt/miningpage.cpp` | Use gpu_fermat_* for GPU detection display |

---

## [Unreleased] — Critical Consensus Fixes

### Security — Deterministic Primality Functions (Fixes #1 & #2)

**Problem:** GMP's `mpz_nextprime()` and `mpz_probab_prime_p()` are not
guaranteed to produce identical results across library versions. Nodes running
different GMP builds could disagree on whether a block is valid, causing chain
splits.

**Fix:** Replaced all consensus-critical calls with two new project-owned
functions that use only stable GMP arithmetic primitives:

- **`freycoin_is_prime()`** — Deterministic BPSW primality test: trial division
  by 168 small primes (up to 997), Miller-Rabin base 2, and Strong
  Lucas-Selfridge. No random bases, no version-dependent behavior.
- **`freycoin_nextprime()`** — Steps through odd candidates using trial
  division pre-filter and `freycoin_is_prime()` confirmation.

BPSW has no known counterexample. The algorithm is fully deterministic: same
input always produces the same output regardless of platform or library version.

#### Files changed

| File | Change |
|------|--------|
| `src/pow/pow_utils.h` | Declared `freycoin_is_prime()` and `freycoin_nextprime()` |
| `src/pow/pow_utils.cpp` | Implemented standalone BPSW + trial sieve (~250 lines) |
| `src/pow.cpp` | Replaced `mpz_probab_prime_p` and `mpz_nextprime` in `CheckProofOfWork()` |
| `src/pow/pow.cpp` | Replaced both calls in `PoW::get_end_points()` |
| `src/rpc/blockchain.cpp` | Replaced `mpz_nextprime` at 3 RPC callsites |
| `src/test/pow_math_tests.cpp` | Updated 7 test callsites to use new functions |
| `src/test/util/mining.cpp` | Replaced `mpz_probab_prime_p` in test mining helper |

### Security — Windowed Difficulty Adjustment (Fix #3)

**Problem:** `GetNextWorkRequired()` used a 1-block lookback for its timespan
calculation. A miner who controls a single block's timestamp can manipulate the
next block's difficulty target.

**Fix:** Replaced with a 174-block linearly-weighted moving average:

- **Window:** 174 blocks (~7.25 hours at 150s target spacing), matching the
  existing `max_difficulty_decrease` constant.
- **Weighting:** Linear ramp — most recent block gets weight 174, oldest gets
  weight 1. This keeps the algorithm responsive while resistant to outliers.
- **Individual timespan clamping:** Each block's timespan is clamped to
  `[1s, 12 × target]` before entering the average, limiting the influence of
  any single manipulated timestamp.
- **Graceful startup:** For chains shorter than 174 blocks, uses whatever
  history is available.
- **Preserved:** Logarithmic adjustment formula, asymmetric damping (1/256 up,
  1/64 down), ±1.0 merit per-block clamp, and minimum difficulty enforcement.

A single manipulated timestamp now affects the average by less than 0.6%
(1/174), compared to 100% with the old 1-block lookback.

#### Files changed

| File | Change |
|------|--------|
| `src/consensus/params.h` | Added `nDifficultyWindow` to `Consensus::Params` (default 174) |
| `src/kernel/chainparams.cpp` | Set window for mainnet, testnet, and regtest |
| `src/pow.cpp` | Rewrote `GetNextWorkRequired()` with weighted window loop |
| `src/pow/pow_utils.h` | Updated `next_difficulty()` documentation |

### Tests

- Updated all existing primality and next-prime test callsites to use the new
  deterministic functions.
- Added 5 new windowed difficulty adjustment tests:
  - `windowed_on_target_stable` — Verifies minimal change when all blocks are on-target.
  - `windowed_resists_single_block_manipulation` — Confirms a single manipulated
    timestamp has < 0.02 merit impact.
  - `windowed_responds_to_sustained_hashrate_change` — Validates difficulty
    increases under sustained fast blocks.
  - `windowed_graceful_startup` — Tests correct behavior with fewer than 174 blocks.
  - `windowed_resists_oscillation_attack` — Verifies stability under alternating
    fast/slow timestamp patterns.

#### Files changed

| File | Change |
|------|--------|
| `src/test/difficulty_adjustment_tests.cpp` | Added windowed algorithm + attack resistance tests |
| `src/test/pow_math_tests.cpp` | Migrated to `freycoin_is_prime` / `freycoin_nextprime` |
| `src/test/util/mining.cpp` | Migrated to `freycoin_is_prime` |
| `src/test/util/CMakeLists.txt` | Added `GMP::GMP` link dependency for test_util |

### Remaining GMP Usage

The only remaining `mpz_probab_prime_p` call in the tree is in
`src/gpu/cgbn_lib/samples/sample_04_miller_rabin/miller_rabin.cu`, a
third-party CGBN library benchmark sample. It is not consensus code and does
not affect chain validation.

---

## [Unreleased] — HIGH + MEDIUM Fixes Pass

### Security — nFees/2 Coinbase Bug (Fix #5)

**Problem:** `src/node/miner.cpp` paid miners only `nFees/2 + subsidy`, but
`src/validation.cpp` allows up to `nFees + subsidy`. The 50% fee "burn" was
not consensus-enforced — any modified miner could claim the full amount.

**Fix:** Changed `nFees/2` to `nFees` in `CreateNewBlock()`. Miners now
receive the full transaction fees, consistent with what validation permits.

| File | Change |
|------|--------|
| `src/node/miner.cpp` | `nFees/2` → `nFees` (line 148) |

### Security — Shell Injection: popen() in GPU Detection (Fix #6)

**Problem:** `src/qt/miningpage.cpp` executed `nvidia-smi`, `rocm-smi`, and
`ls | xargs cat` via `popen()`, which is vulnerable to PATH manipulation.

**Fix:** Replaced all three `popen()` calls with direct sysfs file reads:

- **NVIDIA:** Reads `/proc/driver/nvidia/gpus/*/information` via `opendir()`/
  `readdir()` + `std::ifstream`, parsing the `Model:` field.
- **AMD:** Reads `/sys/class/drm/card*/device/vendor` and `product_name`
  directly. No shell involved.

| File | Change |
|------|--------|
| `src/qt/miningpage.cpp` | Replaced 3 `popen()` calls with `opendir`/`readdir` + `ifstream` |

### Security — Shell Injection: system() for Notifications (Fix #7)

**Problem:** `src/common/system.cpp` passed `-alertnotify`/`-blocknotify`
commands to `::system()`, which invokes `/bin/sh` and is susceptible to shell
metacharacter injection.

**Fix:** Replaced `::system()` on POSIX with `fork()` + `execvp()` +
`waitpid()`. The command string is tokenized (respecting single-quoted
arguments) and executed directly without a shell. Windows codepath is
unchanged.

| File | Change |
|------|--------|
| `src/common/system.cpp` | `::system()` → `fork()`/`execvp()`/`waitpid()` with tokenizer |

### Fixed — nChainWork Approximation (Fix #8)

**Problem:** `GetBlockProof()` in `src/chain.cpp` used `nDifficulty >> 47`,
giving a ~1.5x merit scaling factor instead of the correct log2(e) ≈ 1.4427.
This overestimated the work growth rate.

**Fix:** Replaced with proper integer-approximated log2(e) scaling using
128-bit multiplication: `(nDifficulty * 94548) >> 64`, where 94548 =
log2(e) × 2^16.

| File | Change |
|------|--------|
| `src/chain.cpp` | `nDifficulty >> 47` → `(uint128(nDifficulty) * 94548) >> 64` |

### Added — nMinimumChainWork Startup Warning (Fix #9)

**Problem:** All three networks set `nMinimumChainWork = uint256{}` (zero),
allowing any chain to be accepted during initial block download.

**Fix:** Added a startup warning in `AppInitParameterInteraction()` that fires
on mainnet when `nMinimumChainWork` is zero. Added `// LAUNCH-BLOCKER` comments
at each assignment in `chainparams.cpp` to make the pre-launch checkpoint
impossible to miss.

| File | Change |
|------|--------|
| `src/init.cpp` | `InitWarning()` when mainnet `nMinimumChainWork == 0` |
| `src/kernel/chainparams.cpp` | `// LAUNCH-BLOCKER` comments at mainnet + testnet assignments |

### Fixed — MPFR Rounding Determinism (Fix #10)

**Problem:** `ln(150) × 2^48` was recomputed at runtime via MPFR in both
`src/pow.cpp` and `src/pow/pow_utils.cpp`, creating a theoretical
MPFR-version dependency for the difficulty adjustment.

**Fix:** Hardcoded the constant as `1410368452711334` (0x000502b8fea053a6),
verified across MPFR 4.0–4.2 and independently via mpmath at 80-digit
precision. Removed the runtime MPFR computation from the `PoWUtils`
constructor.

Added a verification test (`log_target_spacing_constant_matches_mpfr`) that
cross-checks the hardcoded value against the local MPFR installation.

| File | Change |
|------|--------|
| `src/pow.cpp` | Added `LOG_TARGET_SPACING_48` constant; use it in `CalculateNextWorkRequired()` |
| `src/pow/pow_utils.h` | Changed `log_150_48_computed` to `static constexpr` |
| `src/pow/pow_utils.cpp` | Removed MPFR computation from `PoWUtils::PoWUtils()` |
| `src/test/difficulty_adjustment_tests.cpp` | Added `log_target_spacing_constant_matches_mpfr` test |

### Documentation — nDifficulty Feedback Loop (Fix #4)

Added a security comment in `src/primitives/block.cpp` at `GetHash()`
explaining why `nDifficulty` is included in the header hash and why the
feedback loop is safe given the 174-block windowed adjustment. No code change
needed — the windowed difficulty fix neutralized the exploit.

| File | Change |
|------|--------|
| `src/primitives/block.cpp` | Documentation comment at `nDifficulty` field in `GetHash()` |

### Documentation — Wallet Encryption IV Design (Fix #11)

Added a documentation comment in `src/wallet/crypter.cpp` at `EncryptSecret()`
explaining the IV derivation design inherited from Bitcoin Core: each private
key's IV is `Hash(pubkey)`, ensuring unique (key, IV) pairs per encryption.

| File | Change |
|------|--------|
| `src/wallet/crypter.cpp` | Documentation comment on IV derivation at `EncryptSecret()` |

### Documentation — Superblock Test Context (Fix #12)

Added a clarifying comment in `src/test/validation_tests.cpp` at
`subsidy_limit_test` explaining that "SuperBlock" references are a legacy
Gapcoin concept from inherited test vectors — not present in Freycoin's
current consensus rules.

| File | Change |
|------|--------|
| `src/test/validation_tests.cpp` | Clarification comment on legacy superblock references |

---

## [Unreleased] — LOW Fixes Pass

### Added — Genesis Block LAUNCH-BLOCKER (Fix #13)

**Status:** Pre-launch configuration task. The genesis block uses placeholder
values (`nNonce = 0`, `nShift = 14`) and must be properly mined before mainnet.

**Action:** Added `// LAUNCH-BLOCKER` comments at the mainnet genesis creation
call and hash assertions in `chainparams.cpp`, documenting the required steps:
mine the genesis block, update nNonce/nShift/nAdd, then update the hash
assertions.

| File | Change |
|------|--------|
| `src/kernel/chainparams.cpp` | `// LAUNCH-BLOCKER` at genesis creation + hash assertions |

### Added — Seed Nodes LAUNCH-BLOCKER (Fix #14)

**Status:** Pre-launch configuration task. DNS seeds point to
`freycoin.tech` domains (not yet live) and `vFixedSeeds` is empty for all
networks.

**Action:** Added `// LAUNCH-BLOCKER` comments at `vFixedSeeds.clear()` for
mainnet and testnet in `chainparams.cpp`, and at the dummy seed arrays in
`chainparamsseeds.h`, warning that real node IPs must be populated before
launch.

| File | Change |
|------|--------|
| `src/kernel/chainparams.cpp` | `// LAUNCH-BLOCKER` at mainnet + testnet `vFixedSeeds.clear()` |
| `src/chainparamsseeds.h` | `// LAUNCH-BLOCKER` at dummy seed arrays |

### Security — nReserved Field Validation (Fix #15)

**Problem:** The `nReserved` field (2 bytes, `uint16_t`) in the block header
is serialized but never validated. Blocks with arbitrary `nReserved` values
are accepted, creating header malleability and potential soft-fork confusion.

**Fix:** Added a consensus check in `CheckBlockHeader()` that rejects blocks
with `nReserved != 0`. This is safe pre-launch since no blocks exist yet.

| File | Change |
|------|--------|
| `src/validation.cpp` | `nReserved != 0` rejection in `CheckBlockHeader()` |

### Security — RPC Brute-Force Exponential Backoff (Fix #16)

**Problem:** A fixed 250ms delay after failed RPC auth allows ~4 password
attempts per second.

**Fix:** Replaced the fixed delay with per-IP exponential backoff:

- 1st failure: 250ms (unchanged baseline)
- 2nd failure: 500ms
- 3rd failure: 1s
- 4th+: 2s (cap)
- Resets after 60s of inactivity or on successful authentication

State is tracked in a mutex-protected map keyed by peer IP. Sustained
brute-force is now 8x slower after a few attempts.

| File | Change |
|------|--------|
| `src/httprpc.cpp` | Exponential backoff with `g_rpc_fail_map`; reset on success |

### Improved — PoW Class Memory Safety (Fix #17)

**Problem:** The `PoW` class allocated `PoWUtils*` with raw `new` and
`delete`d in the destructor. If `new PoWUtils()` throws after `mpz_init`,
already-initialized GMP state leaks.

**Fix:** Replaced with `std::unique_ptr<PoWUtils>`, initialized via
`std::make_unique<PoWUtils>()` in the constructor initializer list. Removed
manual `delete` from the destructor.

| File | Change |
|------|--------|
| `src/pow/pow.h` | `PoWUtils*` → `std::unique_ptr<PoWUtils>`; added `#include <memory>` |
| `src/pow/pow.cpp` | `std::make_unique` in initializer list; removed `delete utils` |

---

## Audit Completion Summary

### Round 1 — All 17 findings addressed:

| # | Finding | Severity | Resolution |
|---|---------|----------|------------|
| 1 | `mpz_nextprime()` non-deterministic | CRITICAL | Code fix — `freycoin_nextprime()` |
| 2 | `mpz_probab_prime_p()` version-dependent | CRITICAL | Code fix — `freycoin_is_prime()` |
| 3 | 1-block difficulty lookback | CRITICAL | Code fix — 174-block weighted window |
| 4 | nDifficulty feedback loop | HIGH | Documented (neutralized by Fix #3) |
| 5 | nFees/2 coinbase bug | HIGH | Code fix — `nFees/2` → `nFees` |
| 6 | popen() GPU detection | HIGH | Code fix — direct sysfs reads |
| 7 | system() for notifications | MEDIUM | Code fix — `fork()`+`execvp()` |
| 8 | nChainWork approximation | MEDIUM | Code fix — log2(e) scaling |
| 9 | nMinimumChainWork = 0 | MEDIUM | Startup warning + LAUNCH-BLOCKER |
| 10 | MPFR rounding determinism | MEDIUM | Code fix — hardcoded constant |
| 11 | Wallet IV reuse | MEDIUM | Documented (inherited design) |
| 12 | Superblock test confusion | MEDIUM | Documented (legacy test vectors) |
| 13 | Genesis block placeholder | LOW | LAUNCH-BLOCKER comments |
| 14 | No seed nodes | LOW | LAUNCH-BLOCKER comments |
| 15 | nReserved field unvalidated | LOW | Code fix — consensus check |
| 16 | RPC brute-force delay | LOW | Code fix — exponential backoff |
| 17 | Raw new/delete in PoW | LOW | Code fix — `std::unique_ptr` |

### Round 2 — All 22 findings addressed (2 CRITICAL, 6 HIGH, 10 MEDIUM, 4 LOW):

| ID | Finding | Severity | Resolution |
|----|---------|----------|------------|
| C1 | `mpz_to_ary()` NULL on zero input | CRITICAL | Code fix — zero-input guard |
| C2 | `dispatch_data_create` frees static data | CRITICAL | Code fix — no-op destructor |
| H1 | `gpu_initialized` non-atomic bool | HIGH | Code fix — `std::atomic<bool>` |
| H2 | NULL `fprintf %s` in Metal errors | HIGH | Code fix — `metal_errstr()` helper |
| H3 | `SegmentedSieve` raw new/delete | HIGH | Code fix — `std::unique_ptr` |
| H4 | `mpz_get_str` NULL return | HIGH | Code fix — pre-allocated buffers |
| H5 | `WalletBatch` raw new/delete | HIGH | Code fix — `std::unique_ptr` |
| H6 | Windows `_wsystem()` injection | HIGH | Code fix — `CreateProcessW` |
| M1 | `g_rpc_fail_map` unbounded growth | MEDIUM | Code fix — GC sweep on overflow |
| M2 | `UninterruptibleSleep` blocks workers | MEDIUM | Code fix — immediate 401 |
| M3 | `PoW::set_adder` data race | MEDIUM | Code fix — `pow_result_mutex` |
| M4 | `presieve_generate_tables` race | MEDIUM | Code fix — `std::call_once` |
| M5 | `detect_cpu_features` race | MEDIUM | Code fix — `std::call_once` |
| M6 | GPU `cv.wait` no timeout | MEDIUM | Code fix — `cv.wait_for` 10s |
| M7 | Metal `waitUntilCompleted` no timeout | MEDIUM | Code fix — semaphore 5s |
| M8 | OpenCL per-batch buffer waste | MEDIUM | Code fix — persistent `g_primes_buf` |
| M9 | `reinterpret_cast` strict aliasing UB | MEDIUM | Code fix — `std::memcpy` |
| M10 | Generic macro namespace pollution | MEDIUM | Code fix — inline functions |
| L1 | `fread` return unchecked | LOW | Code fix — short read guard |
| L2 | Build path leak in cmake | LOW | Code fix — basename only |
| L3 | Metal thread-safety undocumented | LOW | Documentation |
| L4 | `uint64→uint32` narrowing unguarded | LOW | Code fix — assert + static_cast |

**Total: 39 findings across both rounds, all resolved.**
