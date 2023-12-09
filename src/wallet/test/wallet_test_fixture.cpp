// Copyright (c) 2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/test/wallet_test_fixture.h>

#include <rpc/server.h>
#include <wallet/rpcwallet.h>

WalletTestingSetup::WalletTestingSetup(const std::string& chainName):
    TestingSetup(chainName), m_wallet(*m_chain, WalletLocation(), WalletDatabase::CreateMock())
{
    g_wallet_allow_fallback_fee = true;

    bool fFirstRun;
    m_wallet.LoadWallet(fFirstRun);
    m_wallet.m_chain_notifications_handler = m_chain->handleNotifications(m_wallet);

    m_chain_client->registerRpcs();
}
