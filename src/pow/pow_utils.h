// Copyright (c) 2014 Jonny Frey <j0nn9.fr39@gmail.com>
// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Proof-of-Work utility functions for merit and difficulty calculations.
 *
 * All logarithmic and exponential computations use MPFR (256-bit precision).
 * No home-grown approximations — every value is correct to the last
 * representable bit.
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#ifndef FREYCOIN_POW_UTILS_H
#define FREYCOIN_POW_UTILS_H

#include <pow/pow_common.h>
#include <cstdint>
#include <vector>
#include <gmp.h>

/*============================================================================
 * Consensus-grade primality functions (deterministic, version-independent)
 *
 * These replace GMP's mpz_probab_prime_p() and mpz_nextprime() for all
 * consensus-critical code paths. GMP's functions are not guaranteed to
 * produce identical results across library versions, which would cause
 * chain splits if nodes run different GMP builds.
 *
 * Algorithm: BPSW (Baillie-PSW) primality test
 *   1. Trial division by primes up to 997
 *   2. Miller-Rabin with deterministic base 2
 *   3. Strong Lucas-Selfridge test (Selfridge Method A parameters)
 *
 * BPSW has no known counterexample. It is deterministic: same input
 * always produces same output regardless of platform or library version.
 *============================================================================*/

/**
 * Deterministic BPSW primality test (consensus-grade).
 *
 * Replaces mpz_probab_prime_p() in all consensus code paths.
 *
 * @param n  The number to test
 * @return 0 if composite, 2 if probably prime (no known BPSW counterexample)
 */
int freycoin_is_prime(const mpz_t n);

/**
 * Deterministic next-prime function (consensus-grade).
 *
 * Replaces mpz_nextprime() in all consensus code paths. Finds the smallest
 * prime strictly greater than n by stepping through odd candidates and
 * testing each with freycoin_is_prime().
 *
 * @param result  Output: the next prime after n (must be mpz_init'd by caller)
 * @param n       Input: find the next prime after this value
 */
void freycoin_nextprime(mpz_t result, const mpz_t n);

/**
 * PoW utility class for merit and difficulty calculations.
 *
 * All fixed-point calculations use 2^48 precision. Logarithms and
 * exponentials are computed via MPFR at 256-bit precision.
 *
 * Key formulas:
 * - merit = gap_size / ln(start)  [2^48 fixed-point]
 * - difficulty = merit + (rand(start, end) % (2/ln(start)))
 * - next_difficulty = current + log(target_spacing / actual_spacing)
 */
class PoWUtils {
public:
    PoWUtils();
    ~PoWUtils();

    // Non-copyable (holds computed state)
    PoWUtils(const PoWUtils&) = delete;
    PoWUtils& operator=(const PoWUtils&) = delete;

    /**
     * Calculate merit of a prime gap.
     * merit = gap_size / ln(start)
     *
     * @param mpz_start Start of gap (must be prime)
     * @param mpz_end End of gap (next prime after start)
     * @return Merit * 2^48 (fixed-point)
     */
    uint64_t merit(mpz_t mpz_start, mpz_t mpz_end);

    /**
     * Generate deterministic random value from gap endpoints.
     * Uses SHA256d(start || end), XOR-folded to 64 bits.
     */
    uint64_t rand(mpz_t mpz_start, mpz_t mpz_end);

    /**
     * Calculate achieved difficulty for a prime gap.
     * difficulty = merit + (rand % (2/ln(start)))
     *
     * The random component provides sub-integer-merit precision.
     */
    uint64_t difficulty(mpz_t mpz_start, mpz_t mpz_end);

    /**
     * Get difficulty in human-readable format (divide by 2^48).
     */
    static double get_readable_difficulty(uint64_t difficulty) {
        return static_cast<double>(difficulty) / TWO_POW48;
    }

    /**
     * Calculate target gap size for given difficulty and start.
     * target_size = difficulty * ln(start)
     */
    uint64_t target_size(mpz_t mpz_start, uint64_t difficulty);

    /**
     * Calculate estimated work (primes to test) for difficulty.
     * work = e^difficulty
     */
    void target_work(std::vector<uint8_t>& n_primes, uint64_t difficulty);

    /**
     * Calculate next difficulty from a single timespan (mining engine helper).
     *
     * This is a single-step adjustment used by the mining engine for
     * estimation. The consensus code (GetNextWorkRequired in pow.cpp)
     * feeds this a linearly-weighted average timespan over 174 blocks.
     *
     * Uses logarithmic adjustment:
     *   next = current + log(target_spacing / actual_spacing) / damping
     *
     * Damping:
     *   - Increases: 1/256 of adjustment (slow up)
     *   - Decreases: 1/64 of adjustment (fast down for recovery)
     *
     * Bounds:
     *   - Maximum change: +/-1.0 merit per block
     *   - Minimum: MIN_DIFFICULTY
     */
    uint64_t next_difficulty(uint64_t difficulty, uint64_t actual_timespan, bool testnet);

    /**
     * Compute maximum possible difficulty decrease in given time.
     */
    static uint64_t max_difficulty_decrease(uint64_t difficulty, int64_t time, bool testnet);

    /**
     * Estimate gaps (blocks) per day for given primes/sec and difficulty.
     */
    double gaps_per_day(double pps, uint64_t difficulty);

    /**
     * Get current time in microseconds.
     */
    static uint64_t gettime_usec();

private:
    // Target block spacing (150 seconds)
    static constexpr int64_t target_spacing = 150;

    // ln(150) * 2^48 — hardcoded consensus constant.
    // Computed via MPFR at 256-bit precision. Verified across MPFR 4.0-4.2 and mpmath.
    // ln(150) = 5.0106352940962555...
    static constexpr uint64_t log_150_48_computed = 1410368452711334ULL;

    // MPFR-based helpers (double-returning, for display/estimation)
    double mpz_log_d(mpz_t mpz);
    double merit_d(mpz_t mpz_start, mpz_t mpz_end);
    double rand_d(mpz_t mpz_start, mpz_t mpz_end);
    double difficulty_d(mpz_t mpz_start, mpz_t mpz_end);
    double next_difficulty_d(double difficulty, uint64_t actual_timespan, bool testnet);
    double target_work_d(uint64_t difficulty);
};

#endif // FREYCOIN_POW_UTILS_H
