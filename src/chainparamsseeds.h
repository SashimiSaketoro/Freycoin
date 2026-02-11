#ifndef FREYCOIN_CHAINPARAMSSEEDS_H
#define FREYCOIN_CHAINPARAMSSEEDS_H
/**
 * List of fixed seed nodes for the Freycoin network
 *
 * Empty for now - Freycoin is a new chain without any deployed nodes yet.
 * Seeds will be added after testnet/mainnet infrastructure is deployed.
 */

// LAUNCH-BLOCKER: Replace these dummy arrays with real seed node IP addresses
// before mainnet/testnet launch. Use contrib/seeds/generate-seeds.py to produce
// the encoded seed data from a list of reachable node IPs.

// Mainnet seeds - empty for new chain (dummy byte for MSVC compatibility)
static const uint8_t chainparams_seed_main[] = {0x00};

// Testnet seeds - empty for new chain (dummy byte for MSVC compatibility)
static const uint8_t chainparams_seed_test[] = {0x00};

#endif // FREYCOIN_CHAINPARAMSSEEDS_H
