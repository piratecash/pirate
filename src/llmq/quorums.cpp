// Copyright (c) 2018-2022 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/quorums.h>
#include <llmq/commitment.h>
#include <llmq/blockprocessor.h>
#include <llmq/dkgsession.h>
#include <llmq/dkgsessionmgr.h>
#include <llmq/utils.h>

#include <evo/specialtx.h>
#include <evo/deterministicmns.h>

#include <masternode/node.h>
#include <chainparams.h>
#include <masternode/sync.h>
#include <net.h>
#include <net_processing.h>
#include <netmessagemaker.h>
#include <univalue.h>
#include <validation.h>

#include <cxxtimer.hpp>

namespace llmq
{

static const std::string DB_QUORUM_SK_SHARE = "q_Qsk";
static const std::string DB_QUORUM_QUORUM_VVEC = "q_Qqvvec";

CQuorumManager* quorumManager;

CCriticalSection cs_data_requests;
static std::unordered_map<std::pair<uint256, bool>, CQuorumDataRequest, StaticSaltedHasher> mapQuorumDataRequests GUARDED_BY(cs_data_requests);

static uint256 MakeQuorumKey(const CQuorum& q)
{
    CHashWriter hw(SER_NETWORK, 0);
    hw << q.params.type;
    hw << q.qc->quorumHash;
    for (const auto& dmn : q.members) {
        hw << dmn->proTxHash;
    }
    return hw.GetHash();
}

CQuorum::CQuorum(const Consensus::LLMQParams& _params, CBLSWorker& _blsWorker) : params(_params), blsCache(_blsWorker)
{
}

CQuorum::~CQuorum() = default;

void CQuorum::Init(const CFinalCommitmentPtr& _qc, const CBlockIndex* _pindexQuorum, const uint256& _minedBlockHash, const std::vector<CDeterministicMNCPtr>& _members)
{
    qc = _qc;
    pindexQuorum = _pindexQuorum;
    members = _members;
    minedBlockHash = _minedBlockHash;
}

bool CQuorum::SetVerificationVector(const BLSVerificationVector& quorumVecIn)
{
    const auto quorumVecInSerialized = ::SerializeHash(quorumVecIn);

    LOCK(cs);
    if (quorumVecInSerialized != qc->quorumVvecHash) {
        return false;
    }
    quorumVvec = std::make_shared<BLSVerificationVector>(quorumVecIn);
    return true;
}

bool CQuorum::SetSecretKeyShare(const CBLSSecretKey& secretKeyShare)
{
    if (!secretKeyShare.IsValid() || (secretKeyShare.GetPublicKey() != GetPubKeyShare(WITH_LOCK(activeMasternodeInfoCs, return GetMemberIndex(activeMasternodeInfo.proTxHash))))) {
        return false;
    }
    LOCK(cs);
    skShare = secretKeyShare;
    return true;
}

bool CQuorum::IsMember(const uint256& proTxHash) const
{
    return ranges::any_of(members, [&proTxHash](const auto& dmn){
        return dmn->proTxHash == proTxHash;
    });
}

bool CQuorum::IsValidMember(const uint256& proTxHash) const
{
    for (size_t i = 0; i < members.size(); i++) {
        if (members[i]->proTxHash == proTxHash) {
            return qc->validMembers[i];
        }
    }
    return false;
}

CBLSPublicKey CQuorum::GetPubKeyShare(size_t memberIdx) const
{
    LOCK(cs);
    if (!HasVerificationVector() || memberIdx >= members.size() || !qc->validMembers[memberIdx]) {
        return CBLSPublicKey();
    }
    auto& m = members[memberIdx];
    return blsCache.BuildPubKeyShare(m->proTxHash, quorumVvec, CBLSId(m->proTxHash));
}

bool CQuorum::HasVerificationVector() const {
    LOCK(cs);
    return quorumVvec != nullptr;
}

CBLSSecretKey CQuorum::GetSkShare() const
{
    LOCK(cs);
    return skShare;
}

int CQuorum::GetMemberIndex(const uint256& proTxHash) const
{
    for (size_t i = 0; i < members.size(); i++) {
        if (members[i]->proTxHash == proTxHash) {
            return (int)i;
        }
    }
    return -1;
}

void CQuorum::WriteContributions(CEvoDB& evoDb) const
{
    uint256 dbKey = MakeQuorumKey(*this);

    LOCK(cs);
    if (HasVerificationVector()) {
        evoDb.GetRawDB().Write(std::make_pair(DB_QUORUM_QUORUM_VVEC, dbKey), *quorumVvec);
    }
    if (skShare.IsValid()) {
        evoDb.GetRawDB().Write(std::make_pair(DB_QUORUM_SK_SHARE, dbKey), skShare);
    }
}

bool CQuorum::ReadContributions(CEvoDB& evoDb)
{
    uint256 dbKey = MakeQuorumKey(*this);

    BLSVerificationVector qv;
    if (evoDb.Read(std::make_pair(DB_QUORUM_QUORUM_VVEC, dbKey), qv)) {
        WITH_LOCK(cs, quorumVvec = std::make_shared<BLSVerificationVector>(std::move(qv)));
    } else {
        return false;
    }

    // We ignore the return value here as it is ok if this fails. If it fails, it usually means that we are not a
    // member of the quorum but observed the whole DKG process to have the quorum verification vector.
    WITH_LOCK(cs, evoDb.Read(std::make_pair(DB_QUORUM_SK_SHARE, dbKey), skShare));

    return true;
}

CQuorumManager::CQuorumManager(CEvoDB& _evoDb, CBLSWorker& _blsWorker, CDKGSessionManager& _dkgManager) :
    evoDb(_evoDb),
    blsWorker(_blsWorker),
    dkgManager(_dkgManager)
{
    CLLMQUtils::InitQuorumsCache(mapQuorumsCache);
    CLLMQUtils::InitQuorumsCache(scanQuorumsCache);
    quorumThreadInterrupt.reset();
}

CQuorumManager::~CQuorumManager()
{
    Stop();
}

void CQuorumManager::Start()
{
    int workerCount = std::thread::hardware_concurrency() / 2;
    workerCount = std::max(std::min(1, workerCount), 4);
    workerPool.resize(workerCount);
    RenameThreadPool(workerPool, "q-mngr");
}

void CQuorumManager::Stop()
{
    quorumThreadInterrupt();
    workerPool.clear_queue();
    workerPool.stop(true);
}

void CQuorumManager::TriggerQuorumDataRecoveryThreads(const CBlockIndex* pIndex) const
{
    if (!fMasternodeMode || !CLLMQUtils::QuorumDataRecoveryEnabled() || pIndex == nullptr) {
        return;
    }

    const std::map<Consensus::LLMQType, QvvecSyncMode> mapQuorumVvecSync = CLLMQUtils::GetEnabledQuorumVvecSyncEntries();

    LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- Process block %s\n", __func__, pIndex->GetBlockHash().ToString());

    for (auto& llmq : Params().GetConsensus().llmqs) {
        // Process signingActiveQuorumCount + 1 quorums for all available llmqTypes
        const auto vecQuorums = ScanQuorums(llmq.first, pIndex, llmq.second.signingActiveQuorumCount + 1);

        // First check if we are member of any quorum of this type
        auto proTxHash = WITH_LOCK(activeMasternodeInfoCs, return activeMasternodeInfo.proTxHash);
        bool fWeAreQuorumTypeMember = ranges::any_of(vecQuorums, [&proTxHash](const auto& pQuorum) {
            return pQuorum->IsValidMember(proTxHash);
        });

        for (const auto& pQuorum : vecQuorums) {
            // If there is already a thread running for this specific quorum skip it
            if (pQuorum->fQuorumDataRecoveryThreadRunning) {
                continue;
            }

            uint16_t nDataMask{0};
            const bool fWeAreQuorumMember = pQuorum->IsValidMember(proTxHash);
            const bool fSyncForTypeEnabled = mapQuorumVvecSync.count(pQuorum->qc->llmqType) > 0;
            const QvvecSyncMode syncMode = fSyncForTypeEnabled ? mapQuorumVvecSync.at(pQuorum->qc->llmqType) : QvvecSyncMode::Invalid;
            const bool fSyncCurrent = syncMode == QvvecSyncMode::Always || (syncMode == QvvecSyncMode::OnlyIfTypeMember && fWeAreQuorumTypeMember);

            if ((fWeAreQuorumMember || (fSyncForTypeEnabled && fSyncCurrent)) && !pQuorum->HasVerificationVector()) {
                nDataMask |= llmq::CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR;
            }

            if (fWeAreQuorumMember && !pQuorum->GetSkShare().IsValid()) {
                nDataMask |= llmq::CQuorumDataRequest::ENCRYPTED_CONTRIBUTIONS;
            }

            if (nDataMask == 0) {
                LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- No data needed from (%d, %s) at height %d\n",
                    __func__, static_cast<uint8_t>(pQuorum->qc->llmqType), pQuorum->qc->quorumHash.ToString(), pIndex->nHeight);
                continue;
            }

            // Finally start the thread which triggers the requests for this quorum
            StartQuorumDataRecoveryThread(pQuorum, pIndex, nDataMask);
        }
    }
}

void CQuorumManager::UpdatedBlockTip(const CBlockIndex* pindexNew, bool fInitialDownload) const
{
    if (!masternodeSync.IsBlockchainSynced()) {
        return;
    }

    for (auto& p : Params().GetConsensus().llmqs) {
        EnsureQuorumConnections(p.first, pindexNew);
    }

    {
        // Cleanup expired data requests
        LOCK(cs_data_requests);
        auto it = mapQuorumDataRequests.begin();
        while (it != mapQuorumDataRequests.end()) {
            if (it->second.IsExpired()) {
                it = mapQuorumDataRequests.erase(it);
            } else {
                ++it;
            }
        }
    }

    TriggerQuorumDataRecoveryThreads(pindexNew);
}

void CQuorumManager::EnsureQuorumConnections(Consensus::LLMQType llmqType, const CBlockIndex* pindexNew) const
{
    const auto& llmq_params = GetLLMQParams(llmqType);

    auto lastQuorums = ScanQuorums(llmqType, pindexNew, (size_t)llmq_params.keepOldConnections);

    auto connmanQuorumsToDelete = g_connman->GetMasternodeQuorums(llmqType);

    // don't remove connections for the currently in-progress DKG round
    int curDkgHeight = pindexNew->nHeight - (pindexNew->nHeight % llmq_params.dkgInterval);
    auto curDkgBlock = pindexNew->GetAncestor(curDkgHeight)->GetBlockHash();
    connmanQuorumsToDelete.erase(curDkgBlock);

    for (const auto& quorum : lastQuorums) {
        if (CLLMQUtils::EnsureQuorumConnections(llmqType, quorum->pindexQuorum, WITH_LOCK(activeMasternodeInfoCs, return activeMasternodeInfo.proTxHash))) {
            continue;
        }
        if (connmanQuorumsToDelete.count(quorum->qc->quorumHash) > 0) {
            LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- removing masternodes quorum connections for quorum %s:\n", __func__, quorum->qc->quorumHash.ToString());
            g_connman->RemoveMasternodeQuorumNodes(llmqType, quorum->qc->quorumHash);
        }
    }
}

CQuorumPtr CQuorumManager::BuildQuorumFromCommitment(const Consensus::LLMQType llmqType, const CBlockIndex* pindexQuorum) const
{
    AssertLockHeld(quorumsCacheCs);
    assert(pindexQuorum);

    const uint256& quorumHash{pindexQuorum->GetBlockHash()};
    uint256 minedBlockHash;
    CFinalCommitmentPtr qc = quorumBlockProcessor->GetMinedCommitment(llmqType, quorumHash, minedBlockHash);
    if (qc == nullptr) {
        return nullptr;
    }
    assert(qc->quorumHash == pindexQuorum->GetBlockHash());

    auto quorum = std::make_shared<CQuorum>(llmq::GetLLMQParams(llmqType), blsWorker);
    auto members = CLLMQUtils::GetAllQuorumMembers(qc->llmqType, pindexQuorum);

    quorum->Init(qc, pindexQuorum, minedBlockHash, members);

    bool hasValidVvec = false;
    if (quorum->ReadContributions(evoDb)) {
        hasValidVvec = true;
    } else {
        if (BuildQuorumContributions(qc, quorum)) {
            quorum->WriteContributions(evoDb);
            hasValidVvec = true;
        } else {
            LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- quorum.ReadContributions and BuildQuorumContributions for block %s failed\n", __func__, qc->quorumHash.ToString());
        }
    }

    if (hasValidVvec) {
        // pre-populate caches in the background
        // recovering public key shares is quite expensive and would result in serious lags for the first few signing
        // sessions if the shares would be calculated on-demand
        StartCachePopulatorThread(quorum);
    }

    mapQuorumsCache[llmqType].insert(quorumHash, quorum);

    return quorum;
}

bool CQuorumManager::BuildQuorumContributions(const CFinalCommitmentPtr& fqc, const std::shared_ptr<CQuorum>& quorum) const
{
    std::vector<uint16_t> memberIndexes;
    std::vector<BLSVerificationVectorPtr> vvecs;
    BLSSecretKeyVector skContributions;
    if (!dkgManager.GetVerifiedContributions((Consensus::LLMQType)fqc->llmqType, quorum->pindexQuorum, fqc->validMembers, memberIndexes, vvecs, skContributions)) {
        return false;
    }

    cxxtimer::Timer t2(true);
    LOCK(quorum->cs);
    quorum->quorumVvec = blsWorker.BuildQuorumVerificationVector(vvecs);
    if (!quorum->HasVerificationVector()) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- failed to build quorumVvec\n", __func__);
        // without the quorum vvec, there can't be a skShare, so we fail here. Failure is not fatal here, as it still
        // allows to use the quorum as a non-member (verification through the quorum pub key)
        return false;
    }
    quorum->skShare = blsWorker.AggregateSecretKeys(skContributions);
    if (!quorum->skShare.IsValid()) {
        quorum->skShare.Reset();
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- failed to build skShare\n", __func__);
        // We don't bail out here as this is not a fatal error and still allows us to recover public key shares (as we
        // have a valid quorum vvec at this point)
    }
    t2.stop();

    LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- built quorum vvec and skShare. time=%d\n", __func__, t2.count());

    return true;
}

bool CQuorumManager::HasQuorum(Consensus::LLMQType llmqType, const uint256& quorumHash)
{
    return quorumBlockProcessor->HasMinedCommitment(llmqType, quorumHash);
}

bool CQuorumManager::RequestQuorumData(CNode* pFrom, Consensus::LLMQType llmqType, const CBlockIndex* pQuorumIndex, uint16_t nDataMask, const uint256& proTxHash) const
{
    if (pFrom == nullptr) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- Invalid pFrom: nullptr\n", __func__);
        return false;
    }
    if (pFrom->nVersion < LLMQ_DATA_MESSAGES_VERSION) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- Version must be %d or greater.\n", __func__, LLMQ_DATA_MESSAGES_VERSION);
        return false;
    }
    if (pFrom->GetVerifiedProRegTxHash().IsNull() && !pFrom->qwatch) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- pFrom is neither a verified masternode nor a qwatch connection\n", __func__);
        return false;
    }
    if (Params().GetConsensus().llmqs.count(llmqType) == 0) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- Invalid llmqType: %d\n", __func__, static_cast<uint8_t>(llmqType));
        return false;
    }
    if (pQuorumIndex == nullptr) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- Invalid pQuorumIndex: nullptr\n", __func__);
        return false;
    }
    if (GetQuorum(llmqType, pQuorumIndex) == nullptr) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- Quorum not found: %s, %d\n", __func__, pQuorumIndex->GetBlockHash().ToString(), static_cast<uint8_t>(llmqType));
        return false;
    }

    LOCK(cs_data_requests);
    auto key = std::make_pair(pFrom->GetVerifiedProRegTxHash(), true);
    auto it = mapQuorumDataRequests.emplace(key, CQuorumDataRequest(llmqType, pQuorumIndex->GetBlockHash(), nDataMask, proTxHash));
    if (!it.second && !it.first->second.IsExpired()) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- Already requested\n", __func__);
        return false;
    }

    CNetMsgMaker msgMaker(pFrom->GetSendVersion());
    g_connman->PushMessage(pFrom, msgMaker.Make(NetMsgType::QGETDATA, it.first->second));

    return true;
}

std::vector<CQuorumCPtr> CQuorumManager::ScanQuorums(Consensus::LLMQType llmqType, size_t nCountRequested) const
{
    const CBlockIndex* pindex = WITH_LOCK(cs_main, return chainActive.Tip());
    return ScanQuorums(llmqType, pindex, nCountRequested);
}

std::vector<CQuorumCPtr> CQuorumManager::ScanQuorums(Consensus::LLMQType llmqType, const CBlockIndex* pindexStart, size_t nCountRequested) const
{
    if (pindexStart == nullptr || nCountRequested == 0) {
        return {};
    }

    bool fCacheExists{false};
    void* pIndexScanCommitments{(void*)pindexStart};
    size_t nScanCommitments{nCountRequested};
    std::vector<CQuorumCPtr> vecResultQuorums;

    {
        LOCK(quorumsCacheCs);
        auto& cache = scanQuorumsCache[llmqType];
        fCacheExists = cache.get(pindexStart->GetBlockHash(), vecResultQuorums);
        if (fCacheExists) {
            // We have exactly what requested so just return it
            if (vecResultQuorums.size() == nCountRequested) {
                return vecResultQuorums;
            }
            // If we have more cached than requested return only a subvector
            if (vecResultQuorums.size() > nCountRequested) {
                const std::vector<CQuorumCPtr>& ret = {vecResultQuorums.begin(), vecResultQuorums.begin() + nCountRequested};
                return ret;
            }
            // If we have cached quorums but not enough, subtract what we have from the count and the set correct index where to start
            // scanning for the rests
            if(vecResultQuorums.size() > 0) {
                nScanCommitments -= vecResultQuorums.size();
                pIndexScanCommitments = (void*)vecResultQuorums.back()->pindexQuorum->pprev;
            }
        } else {
            // If there is nothing in cache request at least cache.max_size() because this gets cached then later
            nScanCommitments = std::max(nCountRequested, cache.max_size());
        }
    }
    // Get the block indexes of the mined commitments to build the required quorums from
    auto quorumIndexes = quorumBlockProcessor->GetMinedCommitmentsUntilBlock(llmqType, static_cast<const CBlockIndex*>(pIndexScanCommitments), nScanCommitments);
    vecResultQuorums.reserve(vecResultQuorums.size() + quorumIndexes.size());

    for (auto& quorumIndex : quorumIndexes) {
        assert(quorumIndex);
        auto quorum = GetQuorum(llmqType, quorumIndex);
        assert(quorum != nullptr);
        vecResultQuorums.emplace_back(quorum);
    }

    size_t nCountResult{vecResultQuorums.size()};
    if (nCountResult > 0 && !fCacheExists) {
        LOCK(quorumsCacheCs);
        // Don't cache more than cache.max_size() elements
        auto& cache = scanQuorumsCache[llmqType];
        size_t nCacheEndIndex = std::min(nCountResult, cache.max_size());
        cache.emplace(pindexStart->GetBlockHash(), {vecResultQuorums.begin(), vecResultQuorums.begin() + nCacheEndIndex});
    }
    // Don't return more than nCountRequested elements
    size_t nResultEndIndex = std::min(nCountResult, nCountRequested);
    const std::vector<CQuorumCPtr>& ret = {vecResultQuorums.begin(), vecResultQuorums.begin() + nResultEndIndex};
    return ret;
}

CQuorumCPtr CQuorumManager::GetQuorum(Consensus::LLMQType llmqType, const uint256& quorumHash) const
{
    CBlockIndex* pindexQuorum = WITH_LOCK(cs_main, return LookupBlockIndex(quorumHash));
    if (!pindexQuorum) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- block %s not found\n", __func__, quorumHash.ToString());
        return nullptr;
    }
    return GetQuorum(llmqType, pindexQuorum);
}

CQuorumCPtr CQuorumManager::GetQuorum(Consensus::LLMQType llmqType, const CBlockIndex* pindexQuorum) const
{
    assert(pindexQuorum);

    auto quorumHash = pindexQuorum->GetBlockHash();

    // we must check this before we look into the cache. Reorgs might have happened which would mean we might have
    // cached quorums which are not in the active chain anymore
    if (!HasQuorum(llmqType, quorumHash)) {
        return nullptr;
    }

    LOCK(quorumsCacheCs);
    CQuorumPtr pQuorum;
    if (mapQuorumsCache[llmqType].get(quorumHash, pQuorum)) {
        return pQuorum;
    }

    return BuildQuorumFromCommitment(llmqType, pindexQuorum);
}

size_t CQuorumManager::GetQuorumRecoveryStartOffset(const CQuorumCPtr pQuorum, const CBlockIndex* pIndex) const
{
    auto mns = deterministicMNManager->GetListForBlock(pIndex);
    std::vector<uint256> vecProTxHashes;
    vecProTxHashes.reserve(mns.GetValidMNsCount());
    mns.ForEachMN(true, [&](const CDeterministicMNCPtr& pMasternode) {
        vecProTxHashes.emplace_back(pMasternode->proTxHash);
    });
    std::sort(vecProTxHashes.begin(), vecProTxHashes.end());
    size_t nIndex{0};
    {
        LOCK(activeMasternodeInfoCs);
        for (size_t i = 0; i < vecProTxHashes.size(); ++i) {
            if (activeMasternodeInfo.proTxHash == vecProTxHashes[i]) {
                nIndex = i;
                break;
            }
        }
    }
    return nIndex % pQuorum->qc->validMembers.size();
}

void CQuorumManager::ProcessMessage(CNode* pFrom, const std::string& strCommand, CDataStream& vRecv)
{
    auto strFunc = __func__;
    auto errorHandler = [&](const std::string& strError, int nScore = 10) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- %s: %s, from peer=%d\n", strFunc, strCommand, strError, pFrom->GetId());
        if (nScore > 0) {
            LOCK(cs_main);
            Misbehaving(pFrom->GetId(), nScore);
        }
    };

    if (strCommand == NetMsgType::QGETDATA) {

        if (!fMasternodeMode || pFrom == nullptr || (pFrom->GetVerifiedProRegTxHash().IsNull() && !pFrom->qwatch)) {
            errorHandler("Not a verified masternode or a qwatch connection");
            return;
        }

        CQuorumDataRequest request;
        vRecv >> request;

        auto sendQDATA = [&](CQuorumDataRequest::Errors nError = CQuorumDataRequest::Errors::UNDEFINED,
                             const CDataStream& body = CDataStream(SER_NETWORK, PROTOCOL_VERSION)) {
            request.SetError(nError);
            CDataStream ssResponse(SER_NETWORK, pFrom->GetSendVersion(), request, body);
            g_connman->PushMessage(pFrom, CNetMsgMaker(pFrom->GetSendVersion()).Make(NetMsgType::QDATA, ssResponse));
        };

        {
            LOCK2(cs_main, cs_data_requests);
            auto key = std::make_pair(pFrom->GetVerifiedProRegTxHash(), false);
            auto it = mapQuorumDataRequests.find(key);
            if (it == mapQuorumDataRequests.end()) {
                it = mapQuorumDataRequests.emplace(key, request).first;
            } else if(it->second.IsExpired()) {
                it->second = request;
            } else {
                errorHandler("Request limit exceeded", 25);
            }
        }

        if (Params().GetConsensus().llmqs.count(request.GetLLMQType()) == 0) {
            sendQDATA(CQuorumDataRequest::Errors::QUORUM_TYPE_INVALID);
            return;
        }

        const CBlockIndex* pQuorumIndex = WITH_LOCK(cs_main, return LookupBlockIndex(request.GetQuorumHash()));
        if (pQuorumIndex == nullptr) {
            sendQDATA(CQuorumDataRequest::Errors::QUORUM_BLOCK_NOT_FOUND);
            return;
        }

        const CQuorumCPtr pQuorum = GetQuorum(request.GetLLMQType(), pQuorumIndex);
        if (pQuorum == nullptr) {
            sendQDATA(CQuorumDataRequest::Errors::QUORUM_NOT_FOUND);
            return;
        }

        CDataStream ssResponseData(SER_NETWORK, pFrom->GetSendVersion());

        // Check if request wants QUORUM_VERIFICATION_VECTOR data
        if (request.GetDataMask() & CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR) {
            if (!pQuorum->HasVerificationVector()) {
                sendQDATA(CQuorumDataRequest::Errors::QUORUM_VERIFICATION_VECTOR_MISSING);
                return;
            }

            WITH_LOCK(pQuorum->cs, ssResponseData << *pQuorum->quorumVvec);
        }

        // Check if request wants ENCRYPTED_CONTRIBUTIONS data
        if (request.GetDataMask() & CQuorumDataRequest::ENCRYPTED_CONTRIBUTIONS) {

            int memberIdx = pQuorum->GetMemberIndex(request.GetProTxHash());
            if (memberIdx == -1) {
                sendQDATA(CQuorumDataRequest::Errors::MASTERNODE_IS_NO_MEMBER);
                return;
            }

            std::vector<CBLSIESEncryptedObject<CBLSSecretKey>> vecEncrypted;
            if (!quorumDKGSessionManager->GetEncryptedContributions(request.GetLLMQType(), pQuorumIndex, pQuorum->qc->validMembers, request.GetProTxHash(), vecEncrypted)) {
                sendQDATA(CQuorumDataRequest::Errors::ENCRYPTED_CONTRIBUTIONS_MISSING);
                return;
            }

            ssResponseData << vecEncrypted;
        }

        sendQDATA(CQuorumDataRequest::Errors::NONE, ssResponseData);
        return;
    }

    if (strCommand == NetMsgType::QDATA) {
        auto verifiedProRegTxHash = pFrom->GetVerifiedProRegTxHash();
        if ((!fMasternodeMode && !CLLMQUtils::IsWatchQuorumsEnabled()) || pFrom == nullptr || (verifiedProRegTxHash.IsNull() && !pFrom->qwatch)) {
            errorHandler("Not a verified masternode or a qwatch connection");
            return;
        }

        CQuorumDataRequest request;
        vRecv >> request;

        {
            LOCK2(cs_main, cs_data_requests);
            auto it = mapQuorumDataRequests.find(std::make_pair(verifiedProRegTxHash, true));
            if (it == mapQuorumDataRequests.end()) {
                errorHandler("Not requested");
                return;
            }
            if (it->second.IsProcessed()) {
                errorHandler("Already received");
                return;
            }
            if (request != it->second) {
                errorHandler("Not like requested");
                return;
            }
            it->second.SetProcessed();
        }

        if (request.GetError() != CQuorumDataRequest::Errors::NONE) {
            errorHandler(strprintf("Error %d", request.GetError()), 0);
            return;
        }

        CQuorumPtr pQuorum;
        {
            LOCK(quorumsCacheCs);
            if (!mapQuorumsCache[request.GetLLMQType()].get(request.GetQuorumHash(), pQuorum)) {
                errorHandler("Quorum not found", 0); // Don't bump score because we asked for it
                return;
            }
        }

        // Check if request has QUORUM_VERIFICATION_VECTOR data
        if (request.GetDataMask() & CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR) {

            BLSVerificationVector verificationVector;
            vRecv >> verificationVector;

            if (pQuorum->SetVerificationVector(verificationVector)) {
                StartCachePopulatorThread(pQuorum);
            } else {
                errorHandler("Invalid quorum verification vector");
                return;
            }
        }

        // Check if request has ENCRYPTED_CONTRIBUTIONS data
        if (request.GetDataMask() & CQuorumDataRequest::ENCRYPTED_CONTRIBUTIONS) {

            if (WITH_LOCK(pQuorum->cs, return pQuorum->quorumVvec->size() != pQuorum->params.threshold)) {
                errorHandler("No valid quorum verification vector available", 0); // Don't bump score because we asked for it
                return;
            }

            int memberIdx = pQuorum->GetMemberIndex(request.GetProTxHash());
            if (memberIdx == -1) {
                errorHandler("Not a member of the quorum", 0); // Don't bump score because we asked for it
                return;
            }

            std::vector<CBLSIESEncryptedObject<CBLSSecretKey>> vecEncrypted;
            vRecv >> vecEncrypted;

            BLSSecretKeyVector vecSecretKeys;
            vecSecretKeys.resize(vecEncrypted.size());
            auto secret = WITH_LOCK(activeMasternodeInfoCs, return *activeMasternodeInfo.blsKeyOperator);
            for (size_t i = 0; i < vecEncrypted.size(); ++i) {
                if (!vecEncrypted[i].Decrypt(memberIdx, secret, vecSecretKeys[i], PROTOCOL_VERSION)) {
                    errorHandler("Failed to decrypt");
                    return;
                }
            }

            CBLSSecretKey secretKeyShare = blsWorker.AggregateSecretKeys(vecSecretKeys);
            if (!pQuorum->SetSecretKeyShare(secretKeyShare)) {
                errorHandler("Invalid secret key share received");
                return;
            }
        }
        pQuorum->WriteContributions(evoDb);
        return;
    }
}

void CQuorumManager::StartCachePopulatorThread(const CQuorumCPtr pQuorum) const
{
    if (!pQuorum->HasVerificationVector()) {
        return;
    }

    cxxtimer::Timer t(true);
    LogPrint(BCLog::LLMQ, "CQuorumManager::StartCachePopulatorThread -- start\n");

    // when then later some other thread tries to get keys, it will be much faster
    workerPool.push([pQuorum, t, this](int threadId) {
        for (size_t i = 0; i < pQuorum->members.size() && !quorumThreadInterrupt; i++) {
            if (pQuorum->qc->validMembers[i]) {
                pQuorum->GetPubKeyShare(i);
            }
        }
        LogPrint(BCLog::LLMQ, "CQuorumManager::StartCachePopulatorThread -- done. time=%d\n", t.count());
    });
}

void CQuorumManager::StartQuorumDataRecoveryThread(const CQuorumCPtr pQuorum, const CBlockIndex* pIndex, uint16_t nDataMaskIn) const
{
    if (pQuorum->fQuorumDataRecoveryThreadRunning) {
        LogPrint(BCLog::LLMQ, "CQuorumManager::%s -- Already running\n", __func__);
        return;
    }
    pQuorum->fQuorumDataRecoveryThreadRunning = true;

    workerPool.push([pQuorum, pIndex, nDataMaskIn, this](int threadId) {
        size_t nTries{0};
        uint16_t nDataMask{nDataMaskIn};
        int64_t nTimeLastSuccess{0};
        uint256* pCurrentMemberHash{nullptr};
        std::vector<uint256> vecMemberHashes;
        const size_t nMyStartOffset{GetQuorumRecoveryStartOffset(pQuorum, pIndex)};
        const int64_t nRequestTimeout{10};

        auto printLog = [&](const std::string& strMessage) {
            const std::string strMember{pCurrentMemberHash == nullptr ? "nullptr" : pCurrentMemberHash->ToString()};
            LogPrint(BCLog::LLMQ, "CQuorumManager::StartQuorumDataRecoveryThread -- %s - for llmqType %d, quorumHash %s, nDataMask (%d/%d), pCurrentMemberHash %s, nTries %d\n",
                strMessage, static_cast<uint8_t>(pQuorum->qc->llmqType), pQuorum->qc->quorumHash.ToString(), nDataMask, nDataMaskIn, strMember, nTries);
        };
        printLog("Start");

        while (!masternodeSync.IsBlockchainSynced() && !quorumThreadInterrupt) {
            quorumThreadInterrupt.sleep_for(std::chrono::seconds(nRequestTimeout));
        }

        if (quorumThreadInterrupt) {
            printLog("Aborted");
            return;
        }

        vecMemberHashes.reserve(pQuorum->qc->validMembers.size());
        for (auto& member : pQuorum->members) {
            if (pQuorum->IsValidMember(member->proTxHash) && member->proTxHash != WITH_LOCK(activeMasternodeInfoCs, return activeMasternodeInfo.proTxHash)) {
                vecMemberHashes.push_back(member->proTxHash);
            }
        }
        std::sort(vecMemberHashes.begin(), vecMemberHashes.end());

        printLog("Try to request");

        while (nDataMask > 0 && !quorumThreadInterrupt) {

            if (nDataMask & llmq::CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR &&
                pQuorum->HasVerificationVector()) {
                nDataMask &= ~llmq::CQuorumDataRequest::QUORUM_VERIFICATION_VECTOR;
                printLog("Received quorumVvec");
            }

            if (nDataMask & llmq::CQuorumDataRequest::ENCRYPTED_CONTRIBUTIONS && pQuorum->GetSkShare().IsValid()) {
                nDataMask &= ~llmq::CQuorumDataRequest::ENCRYPTED_CONTRIBUTIONS;
                printLog("Received skShare");
            }

            if (nDataMask == 0) {
                printLog("Success");
                break;
            }

            if ((GetAdjustedTime() - nTimeLastSuccess) > nRequestTimeout) {
                if (nTries >= vecMemberHashes.size()) {
                    printLog("All tried but failed");
                    break;
                }
                // Access the member list of the quorum with the calculated offset applied to balance the load equally
                pCurrentMemberHash = &vecMemberHashes[(nMyStartOffset + nTries++) % vecMemberHashes.size()];
                {
                    LOCK(cs_data_requests);
                    auto it = mapQuorumDataRequests.find(std::make_pair(*pCurrentMemberHash, true));
                    if (it != mapQuorumDataRequests.end() && !it->second.IsExpired()) {
                        printLog("Already asked");
                        continue;
                    }
                }
                // Sleep a bit depending on the start offset to balance out multiple requests to same masternode
                quorumThreadInterrupt.sleep_for(std::chrono::milliseconds(nMyStartOffset * 100));
                nTimeLastSuccess = GetAdjustedTime();
                g_connman->AddPendingMasternode(*pCurrentMemberHash);
                printLog("Connect");
            }

            auto proTxHash = WITH_LOCK(activeMasternodeInfoCs, return activeMasternodeInfo.proTxHash);
            g_connman->ForEachNode([&](CNode* pNode) {
                auto verifiedProRegTxHash = pNode->GetVerifiedProRegTxHash();
                if (pCurrentMemberHash == nullptr || verifiedProRegTxHash != *pCurrentMemberHash) {
                    return;
                }

                if (quorumManager->RequestQuorumData(pNode, pQuorum->qc->llmqType, pQuorum->pindexQuorum, nDataMask, proTxHash)) {
                    nTimeLastSuccess = GetAdjustedTime();
                    printLog("Requested");
                } else {
                    LOCK(cs_data_requests);
                    auto it = mapQuorumDataRequests.find(std::make_pair(verifiedProRegTxHash, true));
                    if (it == mapQuorumDataRequests.end()) {
                        printLog("Failed");
                        pNode->fDisconnect = true;
                        pCurrentMemberHash = nullptr;
                        return;
                    } else if (it->second.IsProcessed()) {
                        printLog("Processed");
                        pNode->fDisconnect = true;
                        pCurrentMemberHash = nullptr;
                        return;
                    } else {
                        printLog("Waiting");
                        return;
                    }
                }
            });
            quorumThreadInterrupt.sleep_for(std::chrono::seconds(1));
        }
        pQuorum->fQuorumDataRecoveryThreadRunning = false;
        printLog("Done");
    });
}

} // namespace llmq
