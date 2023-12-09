// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2014-2020 The Dash Core developers
// Copyright (c) 2020-2022 The Cosanta Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_VALIDATION_H
#define BITCOIN_VALIDATION_H

#if defined(HAVE_CONFIG_H)
#include <config/piratecash-config.h>
#endif

#include <amount.h>
#include <coins.h>
#include <crypto/common.h> // for ReadLE64
#include <fs.h>
#include <protocol.h> // For CMessageHeader::MessageStartChars
#include <policy/feerate.h>
#include <script/script_error.h>
#include <sync.h>
#include <versionbits.h>
#include <spentindex.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <map>
#include <memory>
#include <set>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>
#include <deque>

class CBlockIndex;
class CBlockTreeDB;
class CBlockUndo;
class CChainParams;
class CCoinsViewDB;
class CInv;
class CConnman;
class CScriptCheck;
class CBlockPolicyEstimator;
class CTxMemPool;
class CValidationState;
class PrecomputedTransactionData;
struct ChainTxData;

struct LockPoints;

/** Default for -minrelaytxfee, minimum relay fee for transactions */
static const unsigned int DEFAULT_MIN_RELAY_TX_FEE = 1000;
//! -maxtxfee default
static const CAmount DEFAULT_TRANSACTION_MAXFEE = 0.1 * COIN;
//! Discourage users to set fees higher than this amount (in duffs) per kB
static const CAmount HIGH_TX_FEE_PER_KB = 0.01 * COIN;
//! -maxtxfee will warn if called with a higher fee than this amount (in duffs)
static const CAmount HIGH_MAX_TX_FEE = 100 * HIGH_TX_FEE_PER_KB;
/** Default for -limitancestorcount, max number of in-mempool ancestors */
static const unsigned int DEFAULT_ANCESTOR_LIMIT = 25;
/** Default for -limitancestorsize, maximum kilobytes of tx + all in-mempool ancestors */
static const unsigned int DEFAULT_ANCESTOR_SIZE_LIMIT = 101;
/** Default for -limitdescendantcount, max number of in-mempool descendants */
static const unsigned int DEFAULT_DESCENDANT_LIMIT = 25;
/** Default for -limitdescendantsize, maximum kilobytes of in-mempool descendants */
static const unsigned int DEFAULT_DESCENDANT_SIZE_LIMIT = 101;
/**
 * An extra transaction can be added to a package, as long as it only has one
 * ancestor and is no larger than this. Not really any reason to make this
 * configurable as it doesn't materially change DoS parameters.
 */
static const unsigned int EXTRA_DESCENDANT_TX_SIZE_LIMIT = 10000;
/** Default for -mempoolexpiry, expiration time for mempool transactions in hours */
static const unsigned int DEFAULT_MEMPOOL_EXPIRY = 336;
/** The maximum size of a blk?????.dat file (since 0.8) */
static const unsigned int MAX_BLOCKFILE_SIZE = 0x8000000; // 128 MiB
/** Maximum number of dedicated script-checking threads allowed */
static const int MAX_SCRIPTCHECK_THREADS = 15;
/** -par default (number of script-checking threads, 0 = auto) */
static const int DEFAULT_SCRIPTCHECK_THREADS = 0;
/** Number of headers sent in one getheaders result. We rely on the assumption that if a peer sends
 *  less than this number, we reached its tip. Changing this value is a protocol upgrade. */
static const unsigned int MAX_HEADERS_RESULTS = 2000;
/** Maximum length of reject messages. */
static const unsigned int MAX_REJECT_MESSAGE_LENGTH = 111;

static const int64_t DEFAULT_MAX_TIP_AGE = 6 * 60 * 60; // ~144 blocks behind -> 2 x fork detection time, was 24 * 60 * 60 in bitcoin

/** Default for -permitbaremultisig */
static const bool DEFAULT_PERMIT_BAREMULTISIG = true;
static const bool DEFAULT_CHECKPOINTS_ENABLED = true;
static const bool DEFAULT_TXINDEX = true;
static const bool DEFAULT_ADDRESSINDEX = false;
static const bool DEFAULT_TIMESTAMPINDEX = false;
static const bool DEFAULT_SPENTINDEX = false;
static const char* const DEFAULT_BLOCKFILTERINDEX = "0";
static const unsigned int DEFAULT_BANSCORE_THRESHOLD = 100;
/** Default for -persistmempool */
static const bool DEFAULT_PERSIST_MEMPOOL = true;
/** Default for -syncmempool */
static const bool DEFAULT_SYNC_MEMPOOL = true;

/** Due to high computation requirements for PirateCash PoW & PoS we need to limit message loop blocking */
static constexpr unsigned int MAX_NEW_HEADER_BURST = 50;

/** Default for -stopatheight */
static const int DEFAULT_STOPATHEIGHT = 0;
/** Block files containing a block-height within MIN_BLOCKS_TO_KEEP of chainActive.Tip() will not be pruned. */
static const unsigned int MIN_BLOCKS_TO_KEEP = 288;
static const signed int DEFAULT_CHECKBLOCKS = 6;
static const unsigned int DEFAULT_CHECKLEVEL = 3;

// Require that user allocate at least 945 MiB for block & undo files (blk???.dat and rev???.dat)
// At 2B MiB per block, 288 blocks = 576 MiB.
// Add 15% for Undo data = 662 MiB
// Add 20% for Orphan block rate = 794 MiB
// We want the low water mark after pruning to be at least 794 MiB and since we prune in
// full block file chunks, we need the high water mark which triggers the prune to be
// one 128 MiB block file + added 15% undo data = 147 MiB greater for a total of 941 MiB
// Setting the target to > than 945 MiB will make it likely we can respect the target.
static const uint64_t MIN_DISK_SPACE_FOR_BLOCK_FILES = 945 * 1024 * 1024;

struct BlockHasher
{
    // this used to call `GetCheapHash()` in uint256, which was later moved; the
    // cheap hash function simply calls ReadLE64() however, so the end result is
    // identical
    size_t operator()(const uint256& hash) const { return ReadLE64(hash.begin()); }
};

struct StakeHasher
{
    size_t operator()(const COutPoint& op) const { return ReadLE64(op.hash.begin()) + op.n; }
};

extern CCriticalSection cs_main;
extern CBlockPolicyEstimator feeEstimator;
extern CTxMemPool mempool;
typedef std::unordered_map<uint256, CBlockIndex*, BlockHasher> BlockMap;
typedef std::unordered_multimap<uint256, CBlockIndex*, BlockHasher> PrevBlockMap;
extern BlockMap& mapBlockIndex GUARDED_BY(cs_main);
extern PrevBlockMap& mapPrevBlockIndex;
extern uint64_t nLastBlockTx;
extern uint64_t nLastBlockSize;
extern const std::string strMessageMagic;
extern Mutex g_best_block_mutex;
extern std::condition_variable g_best_block_cv;
extern uint256 g_best_block;
extern std::atomic_bool fImporting;
extern std::atomic_bool fReindex;
extern bool fAddressIndex;
extern bool fTimestampIndex;
extern bool fSpentIndex;
extern bool fIsBareMultisigStd;
/** Whether there are dedicated script-checking threads running.
 * False indicates all script checking is done on the main threadMessageHandler thread.
 */
extern bool g_parallel_script_checks;
extern bool fRequireStandard;
extern bool fCheckBlockIndex;
extern bool fCheckpointsEnabled;
extern size_t nCoinCacheUsage;
/** A fee rate smaller than this is considered zero fee (for relaying, mining and transaction creation) */
extern CFeeRate minRelayTxFee;
/** Absolute maximum transaction fee (in duffs) used by wallet and mempool (rejects high fee in sendrawtransaction) */
extern CAmount maxTxFee;
/** If the tip is older than this (in seconds), the node is considered to be in initial block download. */
extern int64_t nMaxTipAge;

extern bool fLargeWorkForkFound;
extern bool fLargeWorkInvalidChainFound;

extern int64_t nReserveBalance;

extern std::atomic<bool> fDIP0001ActiveAtTip;

/** Block hash whose ancestors we will assume to have valid scripts without checking them. */
extern uint256 hashAssumeValid;

/** Minimum work we will assume exists on some valid chain. */
extern arith_uint256 nMinimumChainWork;

/** Best header we've seen so far (used for getheaders queries' starting points). */
extern CBlockIndex *pindexBestHeader;

/** Pruning-related variables and constants */
/** True if any block files have ever been pruned. */
extern bool fHavePruned;
/** True if we're running in -prune mode. */
extern bool fPruneMode;
/** Number of MiB of block files that we're trying to stay below. */
extern uint64_t nPruneTarget;

extern uint32_t nFirstPoSBlock;
extern uint32_t nlastPoWBlock;

/**
 * Process an incoming block. This only returns after the best known valid
 * block is made active. Note that it does not, however, guarantee that the
 * specific block passed to it has been checked for validity!
 *
 * If you want to *possibly* get feedback on whether pblock is valid, you must
 * install a CValidationInterface (see validationinterface.h) - this will have
 * its BlockChecked method called whenever *any* block completes validation.
 *
 * Note that we guarantee that either the proof-of-work is valid on pblock, or
 * (and possibly also) BlockChecked will have been called.
 *
 * May not be called in a
 * validationinterface callback.
 *
 * @param[in]   pblock  The block we want to process.
 * @param[in]   fForceProcessing Process this block even if unrequested; used for non-network block sources and whitelisted peers.
 * @param[out]  fNewBlock A boolean which is set to indicate if the block was first received via this call
 * @return True if state.IsValid()
 */
bool ProcessNewBlock(const CChainParams& chainparams, const std::shared_ptr<const CBlock> pblock, bool fForceProcessing, bool* fNewBlock) LOCKS_EXCLUDED(cs_main);

/**
 * Process incoming block headers.
 *
 * May not be called in a
 * validationinterface callback.
 *
 * @param[in]  block The block headers themselves
 * @param[out] state This may be set to an Error state if any error occurred processing them
 * @param[in]  chainparams The params for the chain we want to connect to
 * @param[out] ppindex If set, the pointer will be set to point to the last new block index object for the given headers
 * @param[out] first_invalid First header that fails validation, if one exists
 */
bool ProcessNewBlockHeaders(std::deque<CBlockHeader>& headers, CValidationState& state, const CChainParams& chainparams, const CBlockIndex** ppindex=nullptr, CBlockHeader *first_invalid=nullptr, int burst_limit=MAX_HEADERS_RESULTS) LOCKS_EXCLUDED(cs_main);

/** Open a block file (blk?????.dat) */
FILE* OpenBlockFile(const FlatFilePos &pos, bool fReadOnly = false);
/** Translation to a filesystem path */
fs::path GetBlockPosFilename(const FlatFilePos &pos);
/** Import blocks from an external file */
void LoadExternalBlockFile(const CChainParams& chainparams, FILE* fileIn, FlatFilePos* dbp = nullptr);
/** Ensures we have a genesis block in the block tree, possibly writing one to disk. */
bool LoadGenesisBlock(const CChainParams& chainparams);
/** Load the block tree and coins database from disk,
 * initializing state if we're running with -reindex. */
bool LoadBlockIndex(const CChainParams& chainparams) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
/** Update the chain tip based on database information. */
bool LoadChainTip(const CChainParams& chainparams);
/** Unload database information */
void UnloadBlockIndex();
/** Run instances of script checking worker threads */
void StartScriptCheckWorkerThreads(int threads_num);
/** Stop all of the script checking worker threads */
void StopScriptCheckWorkerThreads();
/** Check whether we are doing an initial block download (synchronizing from disk or network) */
bool IsInitialBlockDownload();
/** Retrieve a transaction (from memory pool, or from disk, if possible) */
bool GetTransaction(const uint256& hash, CTransactionRef& tx, const Consensus::Params& params, uint256& hashBlock, bool fAllowSlow = false, CBlockIndex* blockIndex = nullptr);
/**
 * Find the best known block, and make it the tip of the block chain
 *
 * May not be called with cs_main held. May not be called in a
 * validationinterface callback.
 */
bool ActivateBestChain(CValidationState& state, const CChainParams& chainparams, std::shared_ptr<const CBlock> pblock = std::shared_ptr<const CBlock>());

double ConvertBitsToDouble(unsigned int nBits);
CAmount GetBlockSubsidy(int nBits, int nHeight, const Consensus::Params& consensusParams, bool fSuperblockPartOnly = false);
CAmount GetMasternodePayment(int nHeight, CAmount blockValue, int nReallocActivationHeight = std::numeric_limits<int>::max() /* not activated */);

/** Guess verification progress (as a fraction between 0.0=genesis and 1.0=current tip). */
double GuessVerificationProgress(const ChainTxData& data, const CBlockIndex* pindex);

/** Calculate the amount of disk space the block & undo files currently use */
uint64_t CalculateCurrentUsage();

/**
 *  Mark one block file as pruned.
 */
void PruneOneBlockFile(const int fileNumber) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/**
 *  Actually unlink the specified files
 */
void UnlinkPrunedFiles(const std::set<int>& setFilesToPrune);

/** Flush all state, indexes and buffers to disk. */
void FlushStateToDisk();
/** Prune block files and flush state to disk. */
void PruneAndFlush();
/** Prune block files up to a given height */
void PruneBlockFilesManual(int nManualPruneHeight);

/** (try to) add transaction to memory pool */
bool AcceptToMemoryPool(CTxMemPool& pool, CValidationState &state, const CTransactionRef &tx,
                        bool* pfMissingInputs, bool bypass_limits,
                        const CAmount nAbsurdFee, bool test_accept=false) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

bool GetUTXOCoin(const COutPoint& outpoint, Coin& coin);
int GetUTXOHeight(const COutPoint& outpoint);
int GetUTXOConfirmations(const COutPoint& outpoint);

/** Get the BIP9 state for a given deployment at the current tip. */
ThresholdState VersionBitsTipState(const Consensus::Params& params, Consensus::DeploymentPos pos);

/** Get the numerical statistics for the BIP9 state for a given deployment at the current tip. */
BIP9Stats VersionBitsTipStatistics(const Consensus::Params& params, Consensus::DeploymentPos pos);

/** Get the block height at which the BIP9 deployment switched into the state for the block building on the current tip. */
int VersionBitsTipStateSinceHeight(const Consensus::Params& params, Consensus::DeploymentPos pos);

/** Apply the effects of this transaction on the UTXO set represented by view */
void UpdateCoins(const CTransaction& tx, CCoinsViewCache& inputs, int nHeight);

/** Transaction validation functions */

/**
 * Check if transaction will be final in the next block to be created.
 *
 * Calls IsFinalTx() with current block height and appropriate block time.
 *
 * See consensus/consensus.h for flag definitions.
 */
bool CheckFinalTx(const CTransaction &tx, int flags = -1) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/**
 * Test whether the LockPoints height and time are still valid on the current chain
 */
bool TestLockPointValidity(const LockPoints* lp) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/**
 * Check if transaction will be BIP 68 final in the next block to be created.
 *
 * Simulates calling SequenceLocks() with data from the tip of the current active chain.
 * Optionally stores in LockPoints the resulting height and time calculated and the hash
 * of the block needed for calculation or skips the calculation and uses the LockPoints
 * passed in for evaluation.
 * The LockPoints should not be considered valid if CheckSequenceLocks returns false.
 *
 * See consensus/consensus.h for flag definitions.
 */
bool CheckSequenceLocks(const CTxMemPool& pool, const CTransaction& tx, int flags, LockPoints* lp = nullptr, bool useExistingLockPoints = false) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/**
 * Closure representing one script verification
 * Note that this stores references to the spending transaction
 */
class CScriptCheck
{
private:
    CTxOut m_tx_out;
    const CTransaction *ptxTo;
    unsigned int nIn;
    unsigned int nFlags;
    bool cacheStore;
    ScriptError error;
    PrecomputedTransactionData *txdata;

public:
    CScriptCheck(): ptxTo(nullptr), nIn(0), nFlags(0), cacheStore(false), error(SCRIPT_ERR_UNKNOWN_ERROR) {}
    CScriptCheck(const CTxOut& outIn, const CTransaction& txToIn, unsigned int nInIn, unsigned int nFlagsIn, bool cacheIn, PrecomputedTransactionData* txdataIn) :
        m_tx_out(outIn), ptxTo(&txToIn), nIn(nInIn), nFlags(nFlagsIn), cacheStore(cacheIn), error(SCRIPT_ERR_UNKNOWN_ERROR), txdata(txdataIn) { }

    bool operator()();

    void swap(CScriptCheck &check) {
        std::swap(ptxTo, check.ptxTo);
        std::swap(m_tx_out, check.m_tx_out);
        std::swap(nIn, check.nIn);
        std::swap(nFlags, check.nFlags);
        std::swap(cacheStore, check.cacheStore);
        std::swap(error, check.error);
        std::swap(txdata, check.txdata);
    }

    ScriptError GetScriptError() const { return error; }
};

bool GetTimestampIndex(const unsigned int &high, const unsigned int &low, std::vector<uint256> &hashes);
bool GetSpentIndex(CSpentIndexKey &key, CSpentIndexValue &value);
bool GetAddressIndex(uint160 addressHash, int type,
                     std::vector<std::pair<CAddressIndexKey, CAmount> > &addressIndex,
                     int start = 0, int end = 0);
bool GetAddressUnspent(uint160 addressHash, int type,
                       std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > &unspentOutputs);
/** Initializes the script-execution cache */
void InitScriptExecutionCache();


/** Functions for disk access for blocks */
bool ReadBlockFromDisk(CBlock& block, const FlatFilePos& pos, const Consensus::Params& consensusParams);
bool ReadBlockFromDisk(CBlock& block, const CBlockIndex* pindex, const Consensus::Params& consensusParams);

bool UndoReadFromDisk(CBlockUndo& blockundo, const CBlockIndex* pindex);

/** Functions for validating blocks and updating the block tree */

/** Context-independent validity checks */
bool CheckBlock(const CBlock& block, CValidationState& state, const Consensus::Params& consensusParams, bool fCheckProof = true, bool fCheckMerkleRoot = true);

/** Check a block is completely valid from start to finish (only works on top of our current best block) */
bool TestBlockValidity(CValidationState& state, const CChainParams& chainparams, const CBlock& block, CBlockIndex* pindexPrev, bool fChecfCheckProofkPOW = true, bool fCheckMerkleRoot = true) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/** RAII wrapper for VerifyDB: Verify consistency of the block and coin databases */
class CVerifyDB {
public:
    CVerifyDB();
    ~CVerifyDB();
    bool VerifyDB(const CChainParams& chainparams, CCoinsView *coinsview, int nCheckLevel, int nCheckDepth);
};

/** Replay blocks that aren't fully applied to the database. */
bool ReplayBlocks(const CChainParams& params, CCoinsView* view);

inline CBlockIndex* LookupBlockIndex(const uint256& hash)
{
    AssertLockHeld(cs_main);
    BlockMap::const_iterator it = mapBlockIndex.find(hash);
    return it == mapBlockIndex.end() ? nullptr : it->second;
}

/** Find the last common block between the parameter chain and a locator. */
CBlockIndex* FindForkInGlobalIndex(const CChain& chain, const CBlockLocator& locator);

/** Mark a block as precious and reorganize.
 *
 * May not be called in a
 * validationinterface callback.
 */
bool PreciousBlock(CValidationState& state, const CChainParams& params, CBlockIndex *pindex) LOCKS_EXCLUDED(cs_main);

/** Mark a block as invalid. */
bool InvalidateBlock(CValidationState& state, const CChainParams& chainparams, CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/** Mark a block as conflicting. */
bool MarkConflictingBlock(CValidationState& state, const CChainParams& chainparams, CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/** Remove invalidity status from a block and its descendants. */
void ResetBlockFailureFlags(CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/** The currently-connected chain of blocks (protected by cs_main). */
extern CChain& chainActive;

/** Global variable that points to the coins database (protected by cs_main) */
extern std::unique_ptr<CCoinsViewDB> pcoinsdbview;

/** Global variable that points to the active CCoinsView (protected by cs_main) */
extern std::unique_ptr<CCoinsViewCache> pcoinsTip;

/** Global variable that points to the active block tree (protected by cs_main) */
extern std::unique_ptr<CBlockTreeDB> pblocktree;

/**
 * Return the spend height, which is one more than the inputs.GetBestBlock().
 * While checking, GetBestBlock() refers to the parent block. (protected by cs_main)
 * This is also true for mempool checks.
 */
int GetSpendHeight(const CCoinsViewCache& inputs);

extern VersionBitsCache versionbitscache;

/**
 * Determine what nVersion a new block should use.
 */
int32_t ComputeBlockVersion(const CBlockIndex* pindexPrev, const Consensus::Params& params, bool fCheckMasternodesUpgraded = false, bool isPos = false);

/**
 * Return true if hash can be found in chainActive at nBlockHeight height.
 * Fills hashRet with found hash, if no nBlockHeight is specified - chainActive.Height() is used.
 */
bool GetBlockHash(uint256& hashRet, int nBlockHeight = -1);

/** Reject codes greater or equal to this can be returned by AcceptToMemPool
 * for transactions, to signal internal conditions. They cannot and should not
 * be sent over the P2P network.
 */
static const unsigned int REJECT_INTERNAL = 0x100;
/** Too high fee. Can not be triggered by P2P transactions */
static const unsigned int REJECT_HIGHFEE = 0x100;

/** Get block file info entry for one block file */
CBlockFileInfo* GetBlockFileInfo(size_t n);

/** Dump the mempool to disk. */
bool DumpMempool(const CTxMemPool& pool);

/** Load the mempool from disk. */
bool LoadMempool(CTxMemPool& pool);

/** Check if Proof-of-Stake is required for particular height **/
bool IsPoSEnforcedHeight(int nBlockHeight);
bool IsPoSV2EnforcedHeight(int nFirstPoSv2Block);
bool IsPowActiveHeight(int nBlockHeight);

bool CheckProof(CValidationState& state, const CBlockIndex &pindex, const Consensus::Params& params);
bool CheckProof(CValidationState& state, const CBlockHeader &block, const Consensus::Params& params);

//! Check whether the block associated with this index entry is pruned or not.
inline bool IsBlockPruned(const CBlockIndex* pblockindex)
{
    return (fHavePruned && !(pblockindex->nStatus & BLOCK_HAVE_DATA) && pblockindex->nTx > 0);
}

#endif // BITCOIN_VALIDATION_H
