// Copyright (c) 2014 Jonny Frey <j0nn9.fr39@gmail.com>
// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Implementation of PoW utility functions.
 *
 * All logarithmic and exponential computations use MPFR (256-bit precision)
 * for correctness. No home-grown approximations — every block we mine is a
 * mathematical proof, and the math must be exact.
 *
 * In memory of Jonnie Frey (1989-2017), creator of Gapcoin.
 */

#include <pow/pow_utils.h>
#include <crypto/sha256.h>
#include <gmp.h>
#include <mpfr.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

// MPFR precision for all computations (256 bits ≈ 77 decimal digits)
static constexpr mpfr_prec_t MPFR_PRECISION = 256;

/*============================================================================
 * Consensus-grade primality functions
 *
 * These are standalone implementations that do NOT depend on GMP's
 * mpz_probab_prime_p or mpz_nextprime. They use only basic GMP arithmetic
 * operations (mpz_powm, mpz_jacobi, etc.) whose behavior is guaranteed
 * stable across versions.
 *============================================================================*/

/** Small primes for trial division (primes up to 997). */
static const unsigned long SMALL_PRIMES[] = {
    2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53,
    59, 61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107, 109, 113,
    127, 131, 137, 139, 149, 151, 157, 163, 167, 173, 179, 181,
    191, 193, 197, 199, 211, 223, 227, 229, 233, 239, 241, 251,
    257, 263, 269, 271, 277, 281, 283, 293, 307, 311, 313, 317,
    331, 337, 347, 349, 353, 359, 367, 373, 379, 383, 389, 397,
    401, 409, 419, 421, 431, 433, 439, 443, 449, 457, 461, 463,
    467, 479, 487, 491, 499, 503, 509, 521, 523, 541, 547, 557,
    563, 569, 571, 577, 587, 593, 599, 601, 607, 613, 617, 619,
    631, 641, 643, 647, 653, 659, 661, 673, 677, 683, 691, 701,
    709, 719, 727, 733, 739, 743, 751, 757, 761, 769, 773, 787,
    797, 809, 811, 821, 823, 827, 829, 839, 853, 857, 859, 863,
    877, 881, 883, 887, 907, 911, 919, 929, 937, 941, 947, 953,
    967, 971, 977, 983, 991, 997
};
static constexpr int NUM_SMALL_PRIMES = sizeof(SMALL_PRIMES) / sizeof(SMALL_PRIMES[0]);

/**
 * Miller-Rabin primality test with a specific base (deterministic).
 *
 * Tests whether n is a strong probable prime to base a.
 * Uses only basic GMP arithmetic (mpz_powm, comparisons).
 */
static bool consensus_miller_rabin(const mpz_t n, unsigned long base)
{
    // Write n - 1 = d * 2^s with d odd
    mpz_t n_minus_1, d, x, a;
    mpz_init(n_minus_1);
    mpz_init(d);
    mpz_init(x);
    mpz_init(a);

    mpz_sub_ui(n_minus_1, n, 1);
    mpz_set(d, n_minus_1);

    unsigned long s = 0;
    while (mpz_even_p(d)) {
        mpz_tdiv_q_2exp(d, d, 1);
        s++;
    }

    // x = base^d mod n
    mpz_set_ui(a, base);
    mpz_powm(x, a, d, n);

    bool result = false;

    // Check if x == 1 or x == n-1
    if (mpz_cmp_ui(x, 1) == 0 || mpz_cmp(x, n_minus_1) == 0) {
        result = true;
    } else {
        // Square s-1 times
        for (unsigned long r = 1; r < s; r++) {
            mpz_powm_ui(x, x, 2, n);
            if (mpz_cmp(x, n_minus_1) == 0) {
                result = true;
                break;
            }
            if (mpz_cmp_ui(x, 1) == 0) {
                result = false;
                break;
            }
        }
    }

    mpz_clear(n_minus_1);
    mpz_clear(d);
    mpz_clear(x);
    mpz_clear(a);

    return result;
}

/**
 * Find Selfridge parameter D for Strong Lucas test (Method A).
 *
 * Searches D in sequence 5, -7, 9, -11, 13, -15, ... until Jacobi(D, n) = -1.
 * Returns 0 if n has a small factor (detected via Jacobi symbol = 0).
 */
static long consensus_find_selfridge_d(const mpz_t n)
{
    long d = 5;
    long sign = 1;
    mpz_t tmp;
    mpz_init(tmp);

    while (true) {
        mpz_set_si(tmp, d);
        int jacobi = mpz_jacobi(tmp, n);

        if (jacobi == -1) {
            mpz_clear(tmp);
            return d;
        }
        if (jacobi == 0) {
            // n is divisible by |d| (and |d| != n), so n is composite
            if (mpz_cmpabs_ui(n, static_cast<unsigned long>(std::abs(d))) != 0) {
                mpz_clear(tmp);
                return 0;
            }
        }

        sign = -sign;
        d = sign * (std::abs(d) + 2);

        // Safety bound (should never be reached for valid inputs)
        if (std::abs(d) > 1000000) {
            mpz_clear(tmp);
            return 0;
        }
    }
}

/**
 * Strong Lucas-Selfridge primality test (deterministic).
 *
 * Uses Selfridge Method A parameters: P = 1, Q = (1 - D) / 4.
 * Tests whether n is a strong Lucas probable prime.
 */
static bool consensus_strong_lucas_selfridge(const mpz_t n)
{
    // Perfect squares fail Lucas test trivially
    if (mpz_perfect_square_p(n)) return false;

    // Find Selfridge parameter D
    long d_param = consensus_find_selfridge_d(n);
    if (d_param == 0) return false;

    // P = 1, Q = (1 - D) / 4
    long p_param = 1;
    long q_param = (1 - d_param) / 4;

    // Temporaries
    mpz_t tmp, d, u_result, v_result;
    mpz_init(tmp);
    mpz_init(d);
    mpz_init(u_result);
    mpz_init(v_result);

    // Calculate n + 1 = d * 2^s where d is odd
    mpz_add_ui(tmp, n, 1);
    mpz_set(d, tmp);

    unsigned long s = 0;
    while (mpz_even_p(d)) {
        mpz_tdiv_q_2exp(d, d, 1);
        s++;
    }

    // Compute Lucas U_d and V_d using the binary ladder
    mpz_t u_k, v_k, q_k;
    mpz_init_set_ui(u_k, 1);
    mpz_init_set_si(v_k, p_param);
    mpz_init_set_si(q_k, q_param);

    size_t d_bits = mpz_sizeinbase(d, 2);

    for (long i = static_cast<long>(d_bits) - 2; i >= 0; i--) {
        // Double: U_{2k} = U_k * V_k, V_{2k} = V_k^2 - 2*Q^k
        mpz_mul(tmp, u_k, v_k);
        mpz_mod(u_k, tmp, n);

        mpz_mul(tmp, v_k, v_k);
        mpz_submul_ui(tmp, q_k, 2);
        mpz_mod(v_k, tmp, n);

        mpz_mul(q_k, q_k, q_k);
        mpz_mod(q_k, q_k, n);

        // If bit is set, increment index by 1
        if (mpz_tstbit(d, i)) {
            // U_{k+1} = (P*U_k + V_k) / 2
            mpz_mul_si(tmp, u_k, p_param);
            mpz_add(tmp, tmp, v_k);
            if (mpz_odd_p(tmp)) mpz_add(tmp, tmp, n);
            mpz_tdiv_q_2exp(u_result, tmp, 1);
            mpz_mod(u_result, u_result, n);

            // V_{k+1} = (D*U_k + P*V_k) / 2
            mpz_mul_si(tmp, u_k, d_param);
            mpz_addmul_ui(tmp, v_k, static_cast<unsigned long>(p_param));
            if (mpz_odd_p(tmp)) mpz_add(tmp, tmp, n);
            mpz_tdiv_q_2exp(v_result, tmp, 1);
            mpz_mod(v_k, v_result, n);

            mpz_set(u_k, u_result);

            mpz_mul_si(q_k, q_k, q_param);
            mpz_mod(q_k, q_k, n);
        }
    }

    bool is_prime = false;

    // Check U_d = 0 (mod n)
    if (mpz_sgn(u_k) == 0) {
        is_prime = true;
    }
    // Check V_{d*2^r} = 0 (mod n) for r = 0, 1, ..., s-1
    else if (mpz_sgn(v_k) == 0) {
        is_prime = true;
    } else {
        for (unsigned long r = 1; r < s; r++) {
            mpz_mul(tmp, v_k, v_k);
            mpz_submul_ui(tmp, q_k, 2);
            mpz_mod(v_k, tmp, n);

            if (mpz_sgn(v_k) == 0) {
                is_prime = true;
                break;
            }

            mpz_mul(q_k, q_k, q_k);
            mpz_mod(q_k, q_k, n);
        }
    }

    mpz_clear(tmp);
    mpz_clear(d);
    mpz_clear(u_result);
    mpz_clear(v_result);
    mpz_clear(u_k);
    mpz_clear(v_k);
    mpz_clear(q_k);

    return is_prime;
}

int freycoin_is_prime(const mpz_t n)
{
    // Handle n < 2
    if (mpz_cmp_ui(n, 2) < 0) return 0;

    // Handle n == 2
    if (mpz_cmp_ui(n, 2) == 0) return 2;

    // Handle even numbers
    if (mpz_even_p(n)) return 0;

    // Trial division by small primes (skip index 0 which is 2, already handled)
    for (int i = 1; i < NUM_SMALL_PRIMES; i++) {
        if (mpz_cmp_ui(n, SMALL_PRIMES[i]) == 0) return 2;
        if (mpz_divisible_ui_p(n, SMALL_PRIMES[i])) return 0;
    }

    // Miller-Rabin with deterministic base 2
    if (!consensus_miller_rabin(n, 2)) return 0;

    // Strong Lucas-Selfridge test
    if (!consensus_strong_lucas_selfridge(n)) return 0;

    // Passed BPSW — probably prime (no known counterexample)
    return 2;
}

void freycoin_nextprime(mpz_t result, const mpz_t n)
{
    mpz_t candidate;
    mpz_init(candidate);

    // Start at n + 1; if that's even, go to n + 2
    mpz_add_ui(candidate, n, 1);
    if (mpz_even_p(candidate)) {
        mpz_add_ui(candidate, candidate, 1);
    }

    // Step through odd numbers until we find a prime
    while (true) {
        // Quick trial division pre-filter (skip 2, already guaranteed odd)
        bool trial_composite = false;
        for (int i = 1; i < NUM_SMALL_PRIMES; i++) {
            if (mpz_cmp_ui(candidate, SMALL_PRIMES[i]) == 0) {
                // candidate IS this small prime — it's prime
                trial_composite = false;
                break;
            }
            if (mpz_divisible_ui_p(candidate, SMALL_PRIMES[i])) {
                trial_composite = true;
                break;
            }
        }

        if (!trial_composite && freycoin_is_prime(candidate)) {
            mpz_set(result, candidate);
            mpz_clear(candidate);
            return;
        }

        mpz_add_ui(candidate, candidate, 2);
    }
}

PoWUtils::PoWUtils() {
    // log_150_48_computed is now a static constexpr in the header.
    // No runtime MPFR computation needed.
}

PoWUtils::~PoWUtils() {
}

uint64_t PoWUtils::gettime_usec() {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

/**
 * Compute ln(src) * 2^precision as an integer using MPFR.
 *
 * This replaces the old mpz_log2-based approach. MPFR's mpfr_log is
 * proven correct to the last bit at any requested precision.
 */
static void mpfr_ln_fixed(mpz_t result, mpz_t src, uint32_t precision) {
    mpfr_t mpfr_src, mpfr_ln;
    mpfr_init2(mpfr_src, MPFR_PRECISION);
    mpfr_init2(mpfr_ln, MPFR_PRECISION);

    mpfr_set_z(mpfr_src, src, MPFR_RNDN);
    mpfr_log(mpfr_ln, mpfr_src, MPFR_RNDN);
    mpfr_mul_2exp(mpfr_ln, mpfr_ln, precision, MPFR_RNDN);
    mpfr_get_z(result, mpfr_ln, MPFR_RNDN);

    mpfr_clear(mpfr_src);
    mpfr_clear(mpfr_ln);
}

uint64_t PoWUtils::merit(mpz_t mpz_start, mpz_t mpz_end) {
    // merit = gap / ln(start), returned as fixed-point * 2^48
    //
    // Computed as: (gap * 2^48) / (ln(start) * 2^48) * 2^48
    // Simplified:  gap * 2^48 / ln_fixed(start, 48)

    mpz_t mpz_gap, mpz_ln, mpz_merit;
    mpz_init(mpz_gap);
    mpz_init(mpz_ln);
    mpz_init(mpz_merit);

    mpz_sub(mpz_gap, mpz_end, mpz_start);

    // ln(start) * 2^48
    mpfr_ln_fixed(mpz_ln, mpz_start, 48);

    // merit_fp48 = gap * 2^48 / ln_fixed
    // But gap is small (fits uint64), so: gap << 48 / ln_fixed
    // Actually: (gap * 2^48) is what we want divided by (ln(start) * 2^48 / 2^48)
    // = gap * 2^48 * 2^48 / (ln(start) * 2^48) = gap * 2^48 / ln(start)
    // Which is: gap * 2^96 / ln_fixed(48)
    mpz_mul_2exp(mpz_merit, mpz_gap, 96);
    mpz_fdiv_q(mpz_merit, mpz_merit, mpz_ln);

    uint64_t result = 0;
    if (mpz_fits_ui64(mpz_merit)) {
        result = mpz_get_ui64(mpz_merit);
    }

    mpz_clear(mpz_gap);
    mpz_clear(mpz_ln);
    mpz_clear(mpz_merit);

    return result;
}

uint64_t PoWUtils::rand(mpz_t mpz_start, mpz_t mpz_end) {
    // Export start and end to byte arrays
    size_t start_len = 0, end_len = 0;
    uint8_t* start_bytes = mpz_to_ary(mpz_start, &start_len);
    uint8_t* end_bytes = mpz_to_ary(mpz_end, &end_len);

    // SHA256(start || end)
    uint8_t tmp[CSHA256::OUTPUT_SIZE];
    CSHA256()
        .Write(start_bytes, start_len)
        .Write(end_bytes, end_len)
        .Finalize(tmp);

    // SHA256(tmp) - double hash
    uint8_t hash[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(tmp, CSHA256::OUTPUT_SIZE).Finalize(hash);

    // XOR-fold 256 bits to 64 bits (using memcpy to avoid strict-aliasing UB)
    uint64_t parts[4];
    std::memcpy(parts, hash, 32);
    uint64_t result = parts[0] ^ parts[1] ^ parts[2] ^ parts[3];

    free(start_bytes);
    free(end_bytes);

    return result;
}

uint64_t PoWUtils::difficulty(mpz_t mpz_start, mpz_t mpz_end) {
    // min_gap_distance_merit = 2 / ln(start), in 2^48 fixed-point
    mpz_t mpz_ln;
    mpz_init(mpz_ln);

    // ln(start) * 2^48
    mpfr_ln_fixed(mpz_ln, mpz_start, 48);

    // 2 * 2^48 * 2^48 / (ln(start) * 2^48) = 2 * 2^48 / ln(start)
    mpz_t mpz_num;
    mpz_init(mpz_num);
    mpz_set_ui(mpz_num, 2);
    mpz_mul_2exp(mpz_num, mpz_num, 96);
    mpz_fdiv_q(mpz_num, mpz_num, mpz_ln);

    uint64_t min_gap_distance_merit = 1;
    if (mpz_fits_ui64(mpz_num)) {
        min_gap_distance_merit = mpz_get_ui64(mpz_num);
    }

    mpz_clear(mpz_ln);
    mpz_clear(mpz_num);

    // difficulty = merit + (rand % min_gap_distance_merit)
    uint64_t m = merit(mpz_start, mpz_end);
    uint64_t r = rand(mpz_start, mpz_end);
    uint64_t diff = m + (r % min_gap_distance_merit);

    return diff;
}

uint64_t PoWUtils::target_size(mpz_t mpz_start, uint64_t difficulty) {
    // target_size = difficulty * ln(start), with difficulty in 2^48 fixed-point
    // = (difficulty / 2^48) * ln(start)
    // = difficulty * ln_fixed(start, 48) / 2^(48 + 48)
    // = difficulty * ln_fixed(start, 48) / 2^96

    mpz_t mpz_ln, mpz_result;
    mpz_init(mpz_ln);
    mpz_init(mpz_result);

    mpfr_ln_fixed(mpz_ln, mpz_start, 48);
    mpz_set_ui64(mpz_result, difficulty);
    mpz_mul(mpz_result, mpz_result, mpz_ln);
    mpz_fdiv_q_2exp(mpz_result, mpz_result, 96);

    uint64_t result = 0;
    if (mpz_fits_ui64(mpz_result)) {
        result = mpz_get_ui64(mpz_result);
    }

    mpz_clear(mpz_ln);
    mpz_clear(mpz_result);

    return result;
}

void PoWUtils::target_work(std::vector<uint8_t>& n_primes, uint64_t difficulty) {
    // work = exp(difficulty / 2^48)
    mpz_t mpz_n_primes;
    mpz_init(mpz_n_primes);

    mpfr_t mpfr_difficulty;
    mpfr_init2(mpfr_difficulty, MPFR_PRECISION);
    mpfr_set_ui(mpfr_difficulty, static_cast<unsigned long>(difficulty >> 32), MPFR_RNDN);
    mpfr_mul_2exp(mpfr_difficulty, mpfr_difficulty, 32, MPFR_RNDN);
    mpfr_add_ui(mpfr_difficulty, mpfr_difficulty, static_cast<unsigned long>(difficulty & 0xffffffff), MPFR_RNDN);
    mpfr_div_2exp(mpfr_difficulty, mpfr_difficulty, 48, MPFR_RNDN);

    mpfr_exp(mpfr_difficulty, mpfr_difficulty, MPFR_RNDN);
    mpfr_get_z(mpz_n_primes, mpfr_difficulty, MPFR_RNDN);

    mpfr_clear(mpfr_difficulty);

    size_t len;
    uint8_t* ary = mpz_to_ary(mpz_n_primes, &len);
    n_primes.assign(ary, ary + len);
    free(ary);

    mpz_clear(mpz_n_primes);
}

uint64_t PoWUtils::next_difficulty(uint64_t difficulty, uint64_t actual_timespan, bool /*testnet*/) {
    // Compute ln(actual_timespan) * 2^48 using MPFR
    mpfr_t mpfr_actual, mpfr_ln;
    mpfr_init2(mpfr_actual, MPFR_PRECISION);
    mpfr_init2(mpfr_ln, MPFR_PRECISION);

    mpfr_set_ui(mpfr_actual, static_cast<unsigned long>(actual_timespan), MPFR_RNDN);
    mpfr_log(mpfr_ln, mpfr_actual, MPFR_RNDN);
    mpfr_mul_2exp(mpfr_ln, mpfr_ln, 48, MPFR_RNDN);

    mpz_t mpz_log_actual_z;
    mpz_init(mpz_log_actual_z);
    mpfr_get_z(mpz_log_actual_z, mpfr_ln, MPFR_RNDN);

    const uint64_t log_target = log_150_48_computed;
    const uint64_t log_actual = mpz_get_ui64(mpz_log_actual_z);

    mpz_clear(mpz_log_actual_z);
    mpfr_clear(mpfr_actual);
    mpfr_clear(mpfr_ln);

    uint64_t next = difficulty;
    uint64_t shift = 8;  // 1/256 for increases

    // Faster correction for hash rate loss
    if (log_actual > log_target) {
        shift = 6;  // 1/64 for decreases
    }

    // Apply adjustment
    if (log_target >= log_actual) {
        uint64_t delta = log_target - log_actual;
        next += delta >> shift;
    } else {
        uint64_t delta = log_actual - log_target;
        if (difficulty >= (delta >> shift)) {
            next -= delta >> shift;
        } else {
            next = MIN_DIFFICULTY;
        }
    }

    // Clamp change to +/-1.0 merit per block
    if (next > difficulty + TWO_POW48) {
        next = difficulty + TWO_POW48;
    }
    if (next < difficulty - TWO_POW48 && difficulty >= TWO_POW48) {
        next = difficulty - TWO_POW48;
    }

    // Enforce minimum
    if (next < MIN_DIFFICULTY) {
        next = MIN_DIFFICULTY;
    }

    return next;
}

uint64_t PoWUtils::max_difficulty_decrease(uint64_t difficulty, int64_t time, bool /*testnet*/) {
    while (time > 0 && difficulty > MIN_DIFFICULTY) {
        if (difficulty >= TWO_POW48) {
            difficulty -= TWO_POW48;
        }
        time -= 26100;  // 174 * 150 seconds
    }

    if (difficulty < MIN_DIFFICULTY) {
        difficulty = MIN_DIFFICULTY;
    }

    return difficulty;
}

double PoWUtils::gaps_per_day(double pps, uint64_t difficulty) {
    return (60.0 * 60.0 * 24.0) / (target_work_d(difficulty) / pps);
}

double PoWUtils::mpz_log_d(mpz_t mpz) {
    mpfr_t mpfr_tmp;
    mpfr_init2(mpfr_tmp, MPFR_PRECISION);
    mpfr_set_z(mpfr_tmp, mpz, MPFR_RNDN);
    mpfr_log(mpfr_tmp, mpfr_tmp, MPFR_RNDN);
    double res = mpfr_get_d(mpfr_tmp, MPFR_RNDN);
    mpfr_clear(mpfr_tmp);
    return res;
}

double PoWUtils::merit_d(mpz_t mpz_start, mpz_t mpz_end) {
    mpz_t mpz_len;
    mpz_init(mpz_len);
    mpz_sub(mpz_len, mpz_end, mpz_start);

    double m = 0.0;
    if (mpz_fits_ui64(mpz_len)) {
        m = static_cast<double>(mpz_get_ui64(mpz_len)) / mpz_log_d(mpz_start);
    }

    mpz_clear(mpz_len);
    return m;
}

double PoWUtils::rand_d(mpz_t mpz_start, mpz_t mpz_end) {
    size_t start_len = 0, end_len = 0;
    uint8_t* start_bytes = mpz_to_ary(mpz_start, &start_len);
    uint8_t* end_bytes = mpz_to_ary(mpz_end, &end_len);

    uint8_t tmp[CSHA256::OUTPUT_SIZE];
    CSHA256()
        .Write(start_bytes, start_len)
        .Write(end_bytes, end_len)
        .Finalize(tmp);

    uint8_t hash[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(tmp, CSHA256::OUTPUT_SIZE).Finalize(hash);

    // XOR-fold 256 bits to 32 bits (using memcpy to avoid strict-aliasing UB)
    uint32_t parts32[8];
    std::memcpy(parts32, hash, 32);
    uint32_t r = parts32[0] ^ parts32[1] ^ parts32[2] ^ parts32[3]
               ^ parts32[4] ^ parts32[5] ^ parts32[6] ^ parts32[7];

    free(start_bytes);
    free(end_bytes);

    return static_cast<double>(r) / static_cast<double>(UINT32_MAX);
}

double PoWUtils::difficulty_d(mpz_t mpz_start, mpz_t mpz_end) {
    double diff = merit_d(mpz_start, mpz_end) +
                  (2.0 / mpz_log_d(mpz_start)) * rand_d(mpz_start, mpz_end);
    return diff < 0.0 ? 0.0 : diff;
}

double PoWUtils::next_difficulty_d(double difficulty, uint64_t actual_timespan, bool /*testnet*/) {
    // Use MPFR for log(150 / actual)
    mpfr_t mpfr_ratio, mpfr_log_ratio;
    mpfr_init2(mpfr_ratio, MPFR_PRECISION);
    mpfr_init2(mpfr_log_ratio, MPFR_PRECISION);

    mpfr_set_d(mpfr_ratio, 150.0 / static_cast<double>(actual_timespan), MPFR_RNDN);
    mpfr_log(mpfr_log_ratio, mpfr_ratio, MPFR_RNDN);
    double log_ratio = mpfr_get_d(mpfr_log_ratio, MPFR_RNDN);

    mpfr_clear(mpfr_ratio);
    mpfr_clear(mpfr_log_ratio);

    uint64_t shift = 8;
    if (actual_timespan > 150) {
        shift = 6;
    }

    double next = difficulty + log_ratio / (1 << shift);

    if (next > difficulty + 1.0) next = difficulty + 1.0;
    if (next < difficulty - 1.0) next = difficulty - 1.0;

    double min_diff = static_cast<double>(MIN_DIFFICULTY) / TWO_POW48;
    if (next < min_diff) next = min_diff;

    return next;
}

double PoWUtils::target_work_d(uint64_t difficulty) {
    // Use MPFR for exp(d)
    mpfr_t mpfr_d, mpfr_result;
    mpfr_init2(mpfr_d, MPFR_PRECISION);
    mpfr_init2(mpfr_result, MPFR_PRECISION);

    mpfr_set_d(mpfr_d, static_cast<double>(difficulty) / TWO_POW48, MPFR_RNDN);
    mpfr_exp(mpfr_result, mpfr_d, MPFR_RNDN);
    double result = mpfr_get_d(mpfr_result, MPFR_RNDN);

    mpfr_clear(mpfr_d);
    mpfr_clear(mpfr_result);

    return result;
}
