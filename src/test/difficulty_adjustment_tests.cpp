// Copyright (c) 2025 The Freycoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Rigorous tests for difficulty adjustment algorithm.
 *
 * Freycoin uses a logarithmic difficulty adjustment with asymmetric damping:
 *   next = current + log(target_spacing / actual_spacing) / damping
 *
 * Key properties that must hold:
 * 1. Stability: On-target blocks cause minimal change
 * 2. Responsiveness: Hash rate changes are tracked
 * 3. Resistance: Gaming attempts are mitigated
 * 4. Bounds: Changes are clamped to prevent instability
 * 5. Minimum: Difficulty never goes below MIN_DIFFICULTY
 *
 * These tests verify the algorithm against attack scenarios and edge cases.
 */

#include <chain.h>
#include <chainparams.h>
#include <pow.h>
#include <pow/pow_utils.h>
#include <pow/pow_common.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>
#include <cmath>
#include <vector>

#include <gmp.h>
#include <mpfr.h>

BOOST_FIXTURE_TEST_SUITE(difficulty_adjustment_tests, BasicTestingSetup)

// Target block spacing in seconds
static constexpr int64_t TARGET_SPACING = 150;

/*============================================================================
 * Basic adjustment behavior
 *============================================================================*/

BOOST_AUTO_TEST_CASE(adjustment_on_target_minimal)
{
    PoWUtils utils;

    // When actual == target, adjustment should be ~0
    std::vector<double> test_difficulties = {10.0, 20.0, 30.0, 50.0, 100.0};

    for (double diff : test_difficulties) {
        uint64_t difficulty = static_cast<uint64_t>(diff * TWO_POW48);
        uint64_t next = utils.next_difficulty(difficulty, TARGET_SPACING, false);

        double delta = std::abs(static_cast<double>(static_cast<int64_t>(next) - static_cast<int64_t>(difficulty))) / TWO_POW48;

        BOOST_CHECK_MESSAGE(delta < 0.001,
            "On-target adjustment for diff=" << diff << " was " << delta << ", expected ~0");
    }
}

BOOST_AUTO_TEST_CASE(adjustment_slow_blocks_decrease)
{
    PoWUtils utils;

    uint64_t difficulty = 20 * TWO_POW48;

    // Slower than target => difficulty should decrease
    for (int64_t time = TARGET_SPACING + 10; time <= TARGET_SPACING * 4; time += 30) {
        uint64_t next = utils.next_difficulty(difficulty, time, false);
        BOOST_CHECK_MESSAGE(next < difficulty,
            "Slow block (" << time << "s) should decrease difficulty");
    }
}

BOOST_AUTO_TEST_CASE(adjustment_fast_blocks_increase)
{
    PoWUtils utils;

    uint64_t difficulty = 20 * TWO_POW48;

    // Faster than target => difficulty should increase
    for (int64_t time = TARGET_SPACING - 10; time >= 10; time -= 10) {
        uint64_t next = utils.next_difficulty(difficulty, time, false);
        BOOST_CHECK_MESSAGE(next > difficulty,
            "Fast block (" << time << "s) should increase difficulty");
    }
}

/*============================================================================
 * Asymmetric damping verification
 *
 * Increases are damped by 1/256 (slow up)
 * Decreases are damped by 1/64 (fast down for recovery)
 *============================================================================*/

BOOST_AUTO_TEST_CASE(damping_asymmetry)
{
    PoWUtils utils;

    uint64_t difficulty = 20 * TWO_POW48;

    // Half target time (75s) => increase
    uint64_t next_fast = utils.next_difficulty(difficulty, 75, false);
    int64_t delta_fast = static_cast<int64_t>(next_fast) - static_cast<int64_t>(difficulty);

    // Double target time (300s) => decrease
    uint64_t next_slow = utils.next_difficulty(difficulty, 300, false);
    int64_t delta_slow = static_cast<int64_t>(next_slow) - static_cast<int64_t>(difficulty);

    // Both have same log magnitude (ln(2)), but different damping
    // Increase: ln(2)/256, Decrease: ln(2)/64
    // So |delta_slow| should be ~4x |delta_fast|

    double ratio = static_cast<double>(-delta_slow) / static_cast<double>(delta_fast);

    BOOST_CHECK_MESSAGE(ratio > 3.5 && ratio < 4.5,
        "Damping ratio = " << ratio << ", expected ~4.0");
}

BOOST_AUTO_TEST_CASE(damping_values_precise)
{
    PoWUtils utils;

    uint64_t difficulty = 20 * TWO_POW48;

    // Block takes half target time (75s)
    // Adjustment = log(150/75) / 256 = ln(2) / 256 ≈ 0.00271
    uint64_t next = utils.next_difficulty(difficulty, 75, false);
    double delta = static_cast<double>(static_cast<int64_t>(next) - static_cast<int64_t>(difficulty)) / TWO_POW48;
    double expected = std::log(2.0) / 256.0;

    BOOST_CHECK_MESSAGE(std::abs(delta - expected) < 0.001,
        "Fast block delta = " << delta << ", expected " << expected);

    // Block takes double target time (300s)
    // Adjustment = log(150/300) / 64 = -ln(2) / 64 ≈ -0.01083
    next = utils.next_difficulty(difficulty, 300, false);
    delta = static_cast<double>(static_cast<int64_t>(next) - static_cast<int64_t>(difficulty)) / TWO_POW48;
    expected = -std::log(2.0) / 64.0;

    BOOST_CHECK_MESSAGE(std::abs(delta - expected) < 0.001,
        "Slow block delta = " << delta << ", expected " << expected);
}

/*============================================================================
 * Clamp verification
 *
 * Maximum change per block is ±1.0 merit.
 *============================================================================*/

BOOST_AUTO_TEST_CASE(clamp_maximum_increase)
{
    PoWUtils utils;

    uint64_t difficulty = 20 * TWO_POW48;

    // Extremely fast block (1 second)
    uint64_t next = utils.next_difficulty(difficulty, 1, false);

    // Maximum increase is +1.0 merit
    BOOST_CHECK_LE(next, difficulty + TWO_POW48);

    // But should still increase
    BOOST_CHECK_GT(next, difficulty);
}

BOOST_AUTO_TEST_CASE(clamp_maximum_decrease)
{
    PoWUtils utils;

    uint64_t difficulty = 20 * TWO_POW48;

    // Extremely slow block (1 hour = 3600s)
    uint64_t next = utils.next_difficulty(difficulty, 3600, false);

    // Maximum decrease is -1.0 merit
    BOOST_CHECK_GE(next, difficulty - TWO_POW48);

    // But should still decrease
    BOOST_CHECK_LT(next, difficulty);
}

BOOST_AUTO_TEST_CASE(clamp_near_minimum)
{
    PoWUtils utils;

    // Difficulty just above minimum
    uint64_t difficulty = MIN_DIFFICULTY + TWO_POW48 / 2;

    // Very slow block
    uint64_t next = utils.next_difficulty(difficulty, 3600, false);

    // Should not go below minimum
    BOOST_CHECK_GE(next, MIN_DIFFICULTY);
}

/*============================================================================
 * Minimum difficulty enforcement
 *============================================================================*/

BOOST_AUTO_TEST_CASE(minimum_enforced_always)
{
    PoWUtils utils;

    // Start at minimum
    uint64_t difficulty = MIN_DIFFICULTY;

    // Even with very slow blocks, shouldn't go below minimum
    for (int i = 0; i < 100; i++) {
        difficulty = utils.next_difficulty(difficulty, 3600, false);
        BOOST_CHECK_GE(difficulty, MIN_DIFFICULTY);
    }
}

BOOST_AUTO_TEST_CASE(minimum_recovery)
{
    PoWUtils utils;

    // Start at minimum
    uint64_t difficulty = MIN_DIFFICULTY;

    // Fast blocks should increase difficulty from minimum
    uint64_t next = utils.next_difficulty(difficulty, 1, false);
    BOOST_CHECK_GT(next, difficulty);
}

/*============================================================================
 * Attack resistance
 *============================================================================*/

BOOST_AUTO_TEST_CASE(resist_timestamp_manipulation_back)
{
    PoWUtils utils;

    uint64_t difficulty = 20 * TWO_POW48;

    // Miner sets timestamp in the past (block appears instant)
    // This should be clamped
    uint64_t next = utils.next_difficulty(difficulty, 0, false);

    // Clamp should prevent massive increase
    BOOST_CHECK_LE(next, difficulty + TWO_POW48);
}

BOOST_AUTO_TEST_CASE(resist_difficulty_oscillation)
{
    PoWUtils utils;

    uint64_t difficulty = 20 * TWO_POW48;

    // Alternating fast and slow blocks
    double total_change = 0;
    for (int i = 0; i < 100; i++) {
        uint64_t prev = difficulty;
        if (i % 2 == 0) {
            difficulty = utils.next_difficulty(difficulty, 75, false);  // Fast
        } else {
            difficulty = utils.next_difficulty(difficulty, 300, false); // Slow
        }
        total_change += std::abs(static_cast<double>(static_cast<int64_t>(difficulty) - static_cast<int64_t>(prev)));
    }

    // Asymmetric damping means net change should be negative
    // (decreases are 4x larger than increases for same magnitude)
    double final_diff = static_cast<double>(difficulty) / TWO_POW48;
    BOOST_CHECK_MESSAGE(final_diff < 20.0,
        "Oscillating blocks should trend downward, got " << final_diff);
}

BOOST_AUTO_TEST_CASE(resist_selfish_mining_incentive)
{
    PoWUtils utils;

    uint64_t difficulty = 20 * TWO_POW48;

    // Simulate selfish miner withholding blocks then releasing
    // 10 blocks withheld = 10 * 150 = 1500s, then released instantly

    // First, 9 "normal" blocks at target
    for (int i = 0; i < 9; i++) {
        difficulty = utils.next_difficulty(difficulty, 150, false);
    }

    // Then the selfish release (appears as 1s block)
    uint64_t before_selfish = difficulty;
    difficulty = utils.next_difficulty(difficulty, 1, false);

    // The increase should be clamped
    BOOST_CHECK_LE(difficulty, before_selfish + TWO_POW48);
}

/*============================================================================
 * Hash rate change simulation
 *============================================================================*/

BOOST_AUTO_TEST_CASE(hashrate_doubles)
{
    PoWUtils utils;

    uint64_t difficulty = 20 * TWO_POW48;

    // Hash rate doubles => blocks come at 75s instead of 150s
    // After enough blocks, difficulty should approximately double

    for (int i = 0; i < 1000; i++) {
        difficulty = utils.next_difficulty(difficulty, 75, false);
    }

    double final_diff = static_cast<double>(difficulty) / TWO_POW48;

    // With damping, won't reach 40.0 immediately, but should be significantly higher
    BOOST_CHECK_MESSAGE(final_diff > 25.0,
        "After 1000 fast blocks, difficulty = " << final_diff << ", expected > 25");
}

BOOST_AUTO_TEST_CASE(hashrate_halves)
{
    PoWUtils utils;

    uint64_t difficulty = 20 * TWO_POW48;

    // Hash rate halves => blocks come at 300s instead of 150s
    // Difficulty should decrease, but not too fast

    for (int i = 0; i < 1000; i++) {
        difficulty = utils.next_difficulty(difficulty, 300, false);
    }

    double final_diff = static_cast<double>(difficulty) / TWO_POW48;

    // Should decrease significantly but not hit minimum
    BOOST_CHECK_MESSAGE(final_diff < 15.0 && final_diff > 5.0,
        "After 1000 slow blocks, difficulty = " << final_diff << ", expected 5-15");
}

BOOST_AUTO_TEST_CASE(hashrate_sudden_loss)
{
    PoWUtils utils;

    uint64_t difficulty = 30 * TWO_POW48;

    // 90% hash rate loss => blocks at 1500s
    // System should recover without getting stuck

    int blocks_to_recover = 0;
    while (difficulty > 5 * TWO_POW48 && blocks_to_recover < 10000) {
        difficulty = utils.next_difficulty(difficulty, 1500, false);
        blocks_to_recover++;
    }

    BOOST_CHECK_MESSAGE(blocks_to_recover < 5000,
        "Recovery from 90% loss took " << blocks_to_recover << " blocks, expected < 5000");
}

/*============================================================================
 * Long-term stability
 *============================================================================*/

BOOST_AUTO_TEST_CASE(equilibrium_reached)
{
    PoWUtils utils;

    uint64_t difficulty = 20 * TWO_POW48;

    // Simulate 10000 blocks at target spacing
    for (int i = 0; i < 10000; i++) {
        difficulty = utils.next_difficulty(difficulty, 150, false);
    }

    double final_diff = static_cast<double>(difficulty) / TWO_POW48;

    // Should be very close to starting value
    BOOST_CHECK_MESSAGE(std::abs(final_diff - 20.0) < 0.1,
        "After 10000 on-target blocks, difficulty = " << final_diff << ", expected ~20.0");
}

BOOST_AUTO_TEST_CASE(random_walk_bounded)
{
    PoWUtils utils;

    uint64_t difficulty = 20 * TWO_POW48;
    uint64_t min_seen = difficulty;
    uint64_t max_seen = difficulty;

    // Simulate blocks with random timing around target
    // Using deterministic "random" for reproducibility
    uint32_t seed = 12345;
    for (int i = 0; i < 10000; i++) {
        // Linear congruential generator
        seed = seed * 1103515245 + 12345;
        // Timing between 100-200s (centered on 150)
        int64_t timing = 100 + (seed % 101);

        difficulty = utils.next_difficulty(difficulty, timing, false);
        min_seen = std::min(min_seen, difficulty);
        max_seen = std::max(max_seen, difficulty);
    }

    double min_d = static_cast<double>(min_seen) / TWO_POW48;
    double max_d = static_cast<double>(max_seen) / TWO_POW48;

    // Should stay within reasonable bounds
    BOOST_CHECK_MESSAGE(min_d > 15.0 && max_d < 25.0,
        "Random walk bounds: [" << min_d << ", " << max_d << "], expected [15, 25]");
}

/*============================================================================
 * max_difficulty_decrease utility function
 *============================================================================*/

BOOST_AUTO_TEST_CASE(max_difficulty_decrease_basic)
{
    // Test the utility function for estimating maximum decrease over time
    uint64_t difficulty = 20 * TWO_POW48;

    // Over 1 hour, maximum decrease is limited
    uint64_t min_after_1h = PoWUtils::max_difficulty_decrease(difficulty, 3600, false);
    BOOST_CHECK_GE(min_after_1h, MIN_DIFFICULTY);
    BOOST_CHECK_LT(min_after_1h, difficulty);

    // Over 1 day, more decrease allowed
    uint64_t min_after_1d = PoWUtils::max_difficulty_decrease(difficulty, 86400, false);
    BOOST_CHECK_LE(min_after_1d, min_after_1h);
}

/*============================================================================
 * Testnet mode (if different behavior)
 *============================================================================*/

BOOST_AUTO_TEST_CASE(testnet_mode)
{
    PoWUtils utils;

    uint64_t difficulty = 5 * TWO_POW48;

    // Testnet should allow lower minimum difficulty
    uint64_t next = utils.next_difficulty(difficulty, 3600, true);

    // Should still respect minimum (may be different for testnet)
    BOOST_CHECK_GE(next, MIN_TEST_DIFFICULTY);
}

/*============================================================================
 * Determinism across calls
 *============================================================================*/

BOOST_AUTO_TEST_CASE(adjustment_deterministic)
{
    PoWUtils utils1, utils2;

    // Same inputs must produce same outputs
    for (int64_t time = 10; time <= 3600; time += 50) {
        for (double diff = 10.0; diff <= 50.0; diff += 5.0) {
            uint64_t difficulty = static_cast<uint64_t>(diff * TWO_POW48);

            uint64_t next1 = utils1.next_difficulty(difficulty, time, false);
            uint64_t next2 = utils2.next_difficulty(difficulty, time, false);

            BOOST_CHECK_EQUAL(next1, next2);
        }
    }
}

/*============================================================================
 * Windowed GetNextWorkRequired tests
 *
 * These tests verify that the 174-block weighted moving average resists
 * single-block timestamp manipulation attacks.
 *============================================================================*/

/** Helper: build a chain of CBlockIndex objects with given timespans. */
static std::vector<CBlockIndex> BuildChain(int height, int64_t start_time,
                                            int64_t spacing, uint64_t difficulty)
{
    std::vector<CBlockIndex> blocks(height + 1);
    for (int i = 0; i <= height; i++) {
        blocks[i].pprev = (i > 0) ? &blocks[i - 1] : nullptr;
        blocks[i].nHeight = i;
        blocks[i].nTime = static_cast<uint32_t>(start_time + i * spacing);
        blocks[i].nDifficulty = difficulty;
    }
    return blocks;
}

BOOST_AUTO_TEST_CASE(windowed_on_target_stable)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& params = chainParams->GetConsensus();

    // Build a chain of 200 blocks all at target spacing (150s)
    uint64_t diff = 20 * TWO_POW48;
    auto blocks = BuildChain(200, 1770668772, params.nPowTargetSpacing, diff);

    uint64_t next = GetNextWorkRequired(&blocks[200], params);

    // Should be very close to current difficulty (on-target)
    double delta = std::abs(static_cast<double>(static_cast<int64_t>(next) - static_cast<int64_t>(diff))) / TWO_POW48;
    BOOST_CHECK_MESSAGE(delta < 0.001,
        "On-target windowed adjustment was " << delta << ", expected ~0");
}

BOOST_AUTO_TEST_CASE(windowed_resists_single_block_manipulation)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& params = chainParams->GetConsensus();

    // Build a chain of 200 blocks at target spacing
    uint64_t diff = 20 * TWO_POW48;
    auto blocks = BuildChain(200, 1770668772, params.nPowTargetSpacing, diff);

    // Save the normal next difficulty
    uint64_t next_normal = GetNextWorkRequired(&blocks[200], params);

    // Now manipulate the LAST block's timestamp to be 1 second after previous
    // (timestamp manipulation attack)
    blocks[200].nTime = blocks[199].nTime + 1;

    uint64_t next_manipulated = GetNextWorkRequired(&blocks[200], params);

    // With 174-block window, a single manipulated block should have minimal
    // effect. The difference should be much less than with 1-block lookback.
    double diff_normal = static_cast<double>(next_normal) / TWO_POW48;
    double diff_manipulated = static_cast<double>(next_manipulated) / TWO_POW48;
    double impact = std::abs(diff_manipulated - diff_normal);

    // Impact should be tiny (< 0.02 merit) because 1 block out of 174 is ~0.6%
    BOOST_CHECK_MESSAGE(impact < 0.02,
        "Single block manipulation impact was " << impact << ", expected < 0.02");
}

BOOST_AUTO_TEST_CASE(windowed_responds_to_sustained_hashrate_change)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& params = chainParams->GetConsensus();

    // Build a chain where blocks come at 75s (2x hashrate)
    uint64_t diff = 20 * TWO_POW48;
    auto blocks = BuildChain(200, 1770668772, 75, diff);

    uint64_t next = GetNextWorkRequired(&blocks[200], params);

    // With sustained fast blocks, difficulty should increase
    BOOST_CHECK_GT(next, diff);
}

BOOST_AUTO_TEST_CASE(windowed_graceful_startup)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& params = chainParams->GetConsensus();

    // Build a short chain (< 174 blocks) — should still work
    uint64_t diff = 20 * TWO_POW48;
    auto blocks = BuildChain(10, 1770668772, params.nPowTargetSpacing, diff);

    uint64_t next = GetNextWorkRequired(&blocks[10], params);

    // Should be close to original difficulty (all on-target)
    double delta = std::abs(static_cast<double>(static_cast<int64_t>(next) - static_cast<int64_t>(diff))) / TWO_POW48;
    BOOST_CHECK_MESSAGE(delta < 0.01,
        "Short chain on-target adjustment was " << delta << ", expected ~0");
}

BOOST_AUTO_TEST_CASE(windowed_resists_oscillation_attack)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    const auto& params = chainParams->GetConsensus();

    // Build a chain where timestamps alternate between 1s and 299s
    // (average = 150s = on target, but trying to game the system)
    uint64_t diff = 20 * TWO_POW48;
    auto blocks = BuildChain(200, 1770668772, 150, diff);

    // Override timestamps to alternate
    for (int i = 1; i <= 200; i++) {
        if (i % 2 == 1) {
            blocks[i].nTime = blocks[i-1].nTime + 1;     // 1 second
        } else {
            blocks[i].nTime = blocks[i-1].nTime + 299;   // 299 seconds
        }
    }

    uint64_t next = GetNextWorkRequired(&blocks[200], params);

    // The weighted average should be close to target (150s), so difficulty
    // should not deviate significantly
    double final_diff = static_cast<double>(next) / TWO_POW48;
    BOOST_CHECK_MESSAGE(std::abs(final_diff - 20.0) < 1.0,
        "Oscillation attack result: " << final_diff << ", expected ~20.0");
}

/**
 * Verify the hardcoded LOG_TARGET_SPACING_48 constant matches MPFR runtime computation.
 *
 * This ensures the constant in src/pow.cpp (1410368452711334) equals ln(150) * 2^48
 * as computed by the local MPFR installation. If this test fails, the local MPFR
 * disagrees with the consensus constant — investigate before deploying.
 */
BOOST_AUTO_TEST_CASE(log_target_spacing_constant_matches_mpfr)
{
    // Hardcoded value from src/pow.cpp
    static constexpr uint64_t LOG_TARGET_SPACING_48 = 1410368452711334ULL;

    // Compute ln(150) * 2^48 via MPFR at 256-bit precision
    mpfr_t val, ln_val;
    mpfr_init2(val, 256);
    mpfr_init2(ln_val, 256);

    mpfr_set_ui(val, 150, MPFR_RNDN);
    mpfr_log(ln_val, val, MPFR_RNDN);
    mpfr_mul_2exp(ln_val, ln_val, 48, MPFR_RNDN);

    mpz_t result;
    mpz_init(result);
    mpfr_get_z(result, ln_val, MPFR_RNDN);

    uint64_t runtime_value = mpz_get_ui(result);

    mpz_clear(result);
    mpfr_clear(val);
    mpfr_clear(ln_val);

    BOOST_CHECK_EQUAL(LOG_TARGET_SPACING_48, runtime_value);
}

BOOST_AUTO_TEST_SUITE_END()
