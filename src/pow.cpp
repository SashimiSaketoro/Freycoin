// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2017 The Bitcoin Core developers
// Copyright (c) 2014-2018 The riecoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <primitives/block.h>
#include <uint256.h>
#include "bignum.h"
#include "util.h"

bool isAfterFork1(int nHeight, const Consensus::Params& params)
{
    if (params.fPowAllowMinDifficultyBlocks) // testnet
    {
        return nHeight > 3000;
    }

    return nHeight > 159000;
}

bool isInSuperblockInterval(int nHeight, const Consensus::Params& params)
{
    return ( (nHeight / params.DifficultyAdjustmentInterval()) % 14) == 12; // once per week
}

bool isSuperblock(int nHeight, const Consensus::Params& params)
{
    return ((nHeight % params.DifficultyAdjustmentInterval()) == 144) && isInSuperblockInterval(nHeight, params);
}

CBigNum nthRoot(CBigNum const &n, int root, CBigNum const &lowerBound)
{
    CBigNum result = lowerBound;
    CBigNum delta = lowerBound/2;

    while (delta >= 1)
    {
        result += delta;
        CBigNum aux = result;
        for (int i = 1; i < root; i++)
            aux *= result;
        if (aux > n)
        {
            result -= delta;
            delta >>= 1;
        }
        else
            delta <<= 1;
    }
    return result;
}

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    // Only change once per difficulty adjustment interval
    if ((pindexLast->nHeight+1) % params.DifficultyAdjustmentInterval() != 0)
    {
        // SuperBlocks
        if (isAfterFork1(pindexLast->nHeight+1, params))
        {
            if (isSuperblock(pindexLast->nHeight+1, params))
            {
                CBigNum bnNewPow;
                bnNewPow.SetCompact(pindexLast->nBits);
                bnNewPow *= 95859; // superblock is 4168/136 times more difficult
                bnNewPow >>= 16; // 95859/65536 ~= (4168/136) ^ 1/9
                LogPrintf("GetNextWorkRequired superblock difficulty:  %d    %08x  %s\n", pindexLast->nHeight+1, bnNewPow.GetCompact(), bnNewPow.getuint256().ToString());
                return bnNewPow.GetCompact();
            }
            else if (isSuperblock(pindexLast->nHeight+1-1, params)) // right after superblock, go back to previous diff
            {
                return pindexLast->pprev->nBits;
            }
        }

        // TestNet
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 2.5 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
//                while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 && pindex->nBits == nProofOfWorkLimit)
                // clo1 - this matches bug in 0.10.2, will always return previous block because it compares with non-compacted size
                // after first min-difficulty block, remaining blocks in interval will also be min-difficulty
                while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 && pindex->nBits == RIECOIN_MIN_PRIME_SIZE)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Go back by what we want to be nTargetTimespan worth of blocks
    int nHeightFirst = pindexLast->nHeight - (params.DifficultyAdjustmentInterval()-1);
    assert(nHeightFirst >= 0);
    if (nHeightFirst == 0)
        nHeightFirst++;
    const CBlockIndex* pindexFirst = pindexLast->GetAncestor(nHeightFirst);
    assert(pindexFirst);

    return CalculateNextWorkRequired(pindexLast, pindexFirst->GetBlockTime(), params);
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (pindexLast->nHeight+1 >= params.DifficultyAdjustmentInterval()*2) {
        if (nActualTimespan < params.nPowTargetTimespan/4)
            nActualTimespan = params.nPowTargetTimespan/4;
        if (nActualTimespan > params.nPowTargetTimespan*4)
            nActualTimespan = params.nPowTargetTimespan*4;
    }

    // Retarget
    const arith_uint256 arPowLimit = UintToArith256(params.powLimit);
    const CBigNum bnPowLimit(arPowLimit);
    CBigNum bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    
    // 9th power (3+RIECOIN_CONSTELLATION_SIZE)
    arith_uint256 arNewPow = GetBlockProof(*pindexLast);
    CBigNum bnNewPow(arNewPow);
 
    bnNewPow = bnNewPow*params.nPowTargetTimespan;
    bnNewPow = bnNewPow/nActualTimespan;
    
    if (isAfterFork1(pindexLast->nHeight+1, params))
    {
        if (isInSuperblockInterval(pindexLast->nHeight+1, params)) // once per week, our interval contains a superblock
        {
            bnNewPow *= 68; // * 136/150 to compensate for difficult superblock
            bnNewPow /= 75;
            LogPrintf("Adjusted because has superblock\n");
        }
        else if (isInSuperblockInterval(pindexLast->nHeight, params))
        {
            bnNewPow *= 75; // * 150/136 to compensate for previous adj
            bnNewPow /= 68;
            LogPrintf("Adjusted because had superblock\n");
        }
    }

    bnNew = nthRoot(bnNewPow, 3+RIECOIN_CONSTELLATION_SIZE, bnNew/2);
    
    if (bnNew < bnPowLimit)
        bnNew = bnPowLimit;
    else if (bnNew > (unsigned long long)-1)
        bnNew = (unsigned long long)-1;
 
    return bnNew.GetCompact();
}

unsigned int generatePrimeBase(CBigNum &bnTarget, uint256 hash, bitsType compactBits)
{
    bnTarget = 1;
    bnTarget <<= ZEROS_BEFORE_HASH_IN_PRIME;
    
    arith_uint256 arHash = UintToArith256(hash);

    for ( int i = 0; i < 256; i++ )
    {
        bnTarget = (bnTarget << 1) + (arHash.GetLow32() & 1);
        arHash >>= 1;
    }
    CBigNum nBits;
    nBits.SetCompact(compactBits);
    if( nBits > nBits.getuint() ) // the protocol stores a compact big int so it supports larger values, but this version of the client does not
    {
        nBits = (unsigned int)-1; // saturate diff at (2**32) - 1, this should be enough for some years ;)
    }
    const unsigned int significativeDigits =  1 + ZEROS_BEFORE_HASH_IN_PRIME + 256;
    unsigned int trailingZeros = nBits.getuint();
    if( trailingZeros < significativeDigits )
        return 0;
    trailingZeros -= significativeDigits;
    bnTarget <<= trailingZeros;
    return trailingZeros;
}


bool CheckProofOfWork(uint256 hash, bitsType compactBits, offsetType delta, const Consensus::Params& params)
{
//LogPrintf("CheckProofOfWork Hash %s\n", hash.ToString().c_str()); // clo1
    
    if (hash == params.hashGenesisBlockForPoW)
        return true;

    CBigNum bnTarget;
    unsigned int trailingZeros = generatePrimeBase(bnTarget, hash, compactBits);

    if ((trailingZeros < 256) && !params.fPowAllowMinDifficultyBlocks)
    {
        arith_uint256 deltaLimit = 1;
        deltaLimit = deltaLimit<<trailingZeros;
        if( delta >= deltaLimit )
            return error("CheckProofOfWork() : candidate larger than allowed %s of %s", delta.ToString().c_str(), deltaLimit.ToString().c_str() );
    }

    CBigNum bigDelta = CBigNum(delta);
    bnTarget += bigDelta;

    if ((bnTarget % 210) != 97)
        return error("CheckProofOfWork() : not valid pow");

    // first we do a single test to quickly discard most of the bogus cases
    if (BN_is_prime_fasttest( &bnTarget, 1, NULL, NULL, NULL, 1) != 1)
    {
        //printf("CheckProofOfWork fail  hash: %s  \ntarget: %d nOffset: %s\n", hash2.GetHex().c_str(), nBits, delta.GetHex().c_str());
        //printf("CheckProofOfWork fail  target: %s  \n", bnTarget.GetHex().c_str());
        return error("CheckProofOfWork() : n not prime");
    }
    bnTarget += 4;
    if (BN_is_prime_fasttest( &bnTarget, 1, NULL, NULL, NULL, 1) != 1)
    {
        return error("CheckProofOfWork() : n+4 not prime");
    }
    bnTarget += 2;
    if (BN_is_prime_fasttest( &bnTarget, 1, NULL, NULL, NULL, 1) != 1)
    {
        return error("CheckProofOfWork() : n+6 not prime");
    }
    bnTarget += 4;
    if (BN_is_prime_fasttest( &bnTarget, 1, NULL, NULL, NULL, 1) != 1)
    {
        return error("CheckProofOfWork() : n+10 not prime");
    }
    bnTarget += 2;
    if (BN_is_prime_fasttest( &bnTarget, 1, NULL, NULL, NULL, 1) != 1)
    {
        return error("CheckProofOfWork() : n+12 not prime");
    }
    bnTarget += 4;
    if (BN_is_prime_fasttest( &bnTarget, 4, NULL, NULL, NULL, 1) != 1)
    {
        return error("CheckProofOfWork() : n+16 not prime");
    }
    bnTarget -= 4;
    if (BN_is_prime_fasttest( &bnTarget, 3, NULL, NULL, NULL, 0) != 1)
    {
        return error("CheckProofOfWork() : n+12 not prime");
    }
    bnTarget -= 2;
    if (BN_is_prime_fasttest( &bnTarget, 3, NULL, NULL, NULL, 0) != 1)
    {
        return error("CheckProofOfWork() : n+10 not prime");
    }
    bnTarget -= 4;
    if (BN_is_prime_fasttest( &bnTarget, 3, NULL, NULL, NULL, 0) != 1)
    {
        return error("CheckProofOfWork() : n+6 not prime");
    }
    bnTarget -= 2;
    if (BN_is_prime_fasttest( &bnTarget, 3, NULL, NULL, NULL, 0) != 1)
    {
        return error("CheckProofOfWork() : n+4 not prime");
    }
    bnTarget -= 4;
    if (BN_is_prime_fasttest( &bnTarget, 3, NULL, NULL, NULL, 0) != 1)
    {
        return error("CheckProofOfWork() : n not prime");
    }
//    bnBestChainLastDiff.SetCompact(compactBits); // clo1
    return true;
}

