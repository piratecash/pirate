// Copyright (c) 2018-2022 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/commitment.h>

#include <evo/deterministicmns.h>
#include <evo/specialtx.h>

#include <chainparams.h>
#include <consensus/validation.h>
#include <logging.h>
#include <validation.h>

namespace llmq
{

CFinalCommitment::CFinalCommitment(const Consensus::LLMQParams& params, const uint256& _quorumHash) :
        llmqType(params.type),
        quorumHash(_quorumHash),
        signers(params.size),
        validMembers(params.size)
{
}

#define LogPrintfFinalCommitment(...) do { \
    LogInstance().LogPrintStr(strprintf("CFinalCommitment::%s -- %s", __func__, tinyformat::format(__VA_ARGS__))); \
} while(0)

bool CFinalCommitment::Verify(const CBlockIndex* pQuorumBaseBlockIndex, bool checkSigs) const
{
    if (nVersion == 0 || nVersion > CURRENT_VERSION) {
        return false;
    }

    if (!Params().GetConsensus().llmqs.count(llmqType)) {
        LogPrintfFinalCommitment("invalid llmqType=%d\n", static_cast<uint8_t>(llmqType));
        return false;
    }
    const auto& llmq_params = GetLLMQParams(llmqType);

    if (!VerifySizes(llmq_params)) {
        return false;
    }

    if (CountValidMembers() < llmq_params.minSize) {
        LogPrintfFinalCommitment("invalid validMembers count. validMembersCount=%d\n", CountValidMembers());
        return false;
    }
    if (CountSigners() < llmq_params.minSize) {
        LogPrintfFinalCommitment("invalid signers count. signersCount=%d\n", CountSigners());
        return false;
    }
    if (!quorumPublicKey.IsValid()) {
        LogPrintfFinalCommitment("invalid quorumPublicKey\n");
        return false;
    }
    if (quorumVvecHash.IsNull()) {
        LogPrintfFinalCommitment("invalid quorumVvecHash\n");
        return false;
    }
    if (!membersSig.IsValid()) {
        LogPrintfFinalCommitment("invalid membersSig\n");
        return false;
    }
    if (!quorumSig.IsValid()) {
        LogPrintfFinalCommitment("invalid vvecSig\n");
        return false;
    }

    auto members = CLLMQUtils::GetAllQuorumMembers(llmq_params, pQuorumBaseBlockIndex);
    for (size_t i = members.size(); i < llmq_params.size; i++) {
        if (validMembers[i]) {
            LogPrintfFinalCommitment("invalid validMembers bitset. bit %d should not be set\n", i);
            return false;
        }
        if (signers[i]) {
            LogPrintfFinalCommitment("invalid signers bitset. bit %d should not be set\n", i);
            return false;
        }
    }

    // sigs are only checked when the block is processed
    if (checkSigs) {
        uint256 commitmentHash = CLLMQUtils::BuildCommitmentHash(llmq_params.type, quorumHash, validMembers, quorumPublicKey, quorumVvecHash);

        std::vector<CBLSPublicKey> memberPubKeys;
        for (size_t i = 0; i < members.size(); i++) {
            if (!signers[i]) {
                continue;
            }
            memberPubKeys.emplace_back(members[i]->pdmnState->pubKeyOperator.Get());
        }

        if (!membersSig.VerifySecureAggregated(memberPubKeys, commitmentHash)) {
            LogPrintfFinalCommitment("invalid aggregated members signature\n");
            return false;
        }

        if (!quorumSig.VerifyInsecure(quorumPublicKey, commitmentHash)) {
            LogPrintfFinalCommitment("invalid quorum signature\n");
            return false;
        }
    }

    return true;
}

bool CFinalCommitment::VerifyNull() const
{
    if (!Params().GetConsensus().llmqs.count(llmqType)) {
        LogPrintfFinalCommitment("invalid llmqType=%d\n", static_cast<uint8_t>(llmqType));
        return false;
    }

    if (!IsNull() || !VerifySizes(GetLLMQParams(llmqType))) {
        return false;
    }

    return true;
}

bool CFinalCommitment::VerifySizes(const Consensus::LLMQParams& params) const
{
    if (signers.size() != params.size) {
        LogPrintfFinalCommitment("invalid signers.size=%d\n", signers.size());
        return false;
    }
    if (validMembers.size() != params.size) {
        LogPrintfFinalCommitment("invalid signers.size=%d\n", signers.size());
        return false;
    }
    return true;
}

bool CheckLLMQCommitment(const CTransaction& tx, const CBlockIndex* pindexPrev, CValidationState& state)
{
    CFinalCommitmentTxPayload qcTx;
    if (!GetTxPayload(tx, qcTx)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-payload");
    }

    if (qcTx.nVersion == 0 || qcTx.nVersion > CFinalCommitmentTxPayload::CURRENT_VERSION) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-version");
    }

    if (qcTx.nHeight != pindexPrev->nHeight + 1) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-height");
    }

    const CBlockIndex* pQuorumBaseBlockIndex = WITH_LOCK(cs_main, return LookupBlockIndex(qcTx.commitment.quorumHash));
    if (!pQuorumBaseBlockIndex) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-quorum-hash");
    }


    if (pQuorumBaseBlockIndex != pindexPrev->GetAncestor(pQuorumBaseBlockIndex->nHeight)) {
        // not part of active chain
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-quorum-hash");
    }

    if (!Params().GetConsensus().llmqs.count(qcTx.commitment.llmqType)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-type");
    }

    if (qcTx.commitment.IsNull()) {
        if (!qcTx.commitment.VerifyNull()) {
            return state.DoS(100, false, REJECT_INVALID, "bad-qc-invalid-null");
        }
        return true;
    }

    if (!qcTx.commitment.Verify(pQuorumBaseBlockIndex, false)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-invalid");
    }

    return true;
}

} // namespace llmq
