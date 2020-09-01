// Copyright (c) 2015 The Bitcoin Core developers
// Copyright (c) 2020-2022 The Cosanta Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ZMQ_ZMQABSTRACTNOTIFIER_H
#define BITCOIN_ZMQ_ZMQABSTRACTNOTIFIER_H

#include <util/memory.h>

#include <memory>
#include <string>

class CBlockIndex;
class CGovernanceObject;
class CGovernanceVote;
class CTransaction;
class CZMQAbstractNotifier;

typedef std::shared_ptr<const CTransaction> CTransactionRef;

namespace llmq {
    class CChainLockSig;
    class CInstantSendLock;
    class CRecoveredSig;
} // namespace llmq

using CZMQNotifierFactory = std::unique_ptr<CZMQAbstractNotifier> (*)();

class CZMQAbstractNotifier
{
public:
    CZMQAbstractNotifier() : psocket(nullptr) { }
    virtual ~CZMQAbstractNotifier();

    template <typename T>
    static std::unique_ptr<CZMQAbstractNotifier> Create()
    {
        return MakeUnique<T>();
    }

    std::string GetType() const { return type; }
    void SetType(const std::string &t) { type = t; }
    std::string GetAddress() const { return address; }
    void SetAddress(const std::string &a) { address = a; }

    virtual bool Initialize(void *pcontext) = 0;
    virtual void Shutdown() = 0;

    virtual bool NotifyBlock(const CBlockIndex *pindex);
    virtual bool NotifyChainLock(const CBlockIndex *pindex, const std::shared_ptr<const llmq::CChainLockSig>& clsig);
    virtual bool NotifyTransaction(const CTransaction &transaction);
    virtual bool NotifyTransactionLock(const CTransactionRef& transaction, const std::shared_ptr<const llmq::CInstantSendLock>& islock);
    virtual bool NotifyGovernanceVote(const std::shared_ptr<const CGovernanceVote>& vote);
    virtual bool NotifyGovernanceObject(const std::shared_ptr<const CGovernanceObject>& object);
    virtual bool NotifyInstantSendDoubleSpendAttempt(const CTransactionRef& currentTx, const CTransactionRef& previousTx);
    virtual bool NotifyRecoveredSig(const std::shared_ptr<const llmq::CRecoveredSig>& sig);

protected:
    void *psocket;
    std::string type;
    std::string address;
};

#endif // BITCOIN_ZMQ_ZMQABSTRACTNOTIFIER_H
