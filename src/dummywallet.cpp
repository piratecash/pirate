// Copyright (c) 2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <logging.h>
#include <util/system.h>
#include <walletinitinterface.h>

#include <stdio.h>

class CWallet;

class DummyWalletInit : public WalletInitInterface {
public:

    bool HasWalletSupport() const override {return false;}
    void AddWalletOptions() const override;
    bool ParameterInteraction() const override {return true;}
    void Construct(InitInterfaces& interfaces) const override {LogPrintf("No wallet support compiled in!\n");}

    // Dash Specific WalletInitInterface InitCoinJoinSettings
    void AutoLockMasternodeCollaterals() const override {}
    void InitCoinJoinSettings() const override {}
    void InitKeePass() const override {}
    bool InitAutoBackup() const override {return true;}
};

void DummyWalletInit::AddWalletOptions() const
{
    gArgs.AddHiddenArgs({
        "-avoidpartialspends",
        "-createwalletbackups=<n>",
        "-disablewallet",
        "-instantsendnotify=<cmd>",
        "-keypool=<n>",
        "-rescan=<mode>",
        "-salvagewallet",
        "-spendzeroconfchange",
        "-upgradewallet",
        "-wallet=<path>",
        "-walletbackupsdir=<dir>",
        "-walletbroadcast",
        "-walletdir=<dir>",
        "-walletnotify=<cmd>",
        "-zapwallettxes=<mode>",
        "-discardfee=<amt>",
        "-fallbackfee=<amt>",
        "-mintxfee=<amt>",
        "-paytxfee=<amt>",
        "-txconfirmtarget=<n>",
        "-hdseed=<hex>",
        "-mnemonic=<text>",
        "-mnemonicpassphrase=<text>",
        "-usehd",
        "-keepass",
        "-keepassid=<id>",
        "-keepasskey=<key>",
        "-keepassname=<name>",
        "-keepassport=<port>",
        "-enablecoinjoin",
        "-coinjoinamount=<n>",
        "-coinjoinautostart",
        "-coinjoindenomsgoal=<n>",
        "-coinjoindenomshardcap=<n>",
        "-coinjoinmultisession",
        "-coinjoinrounds=<n>",
        "-coinjoinsessions=<n>",
        "-dblogsize=<n>",
        "-flushwallet",
        "-privdb",
        "-walletrejectlongchains"
    });
}

const WalletInitInterface& g_wallet_init_interface = DummyWalletInit();

fs::path GetWalletDir()
{
    throw std::logic_error("Wallet function called in non-wallet build.");
}

std::vector<fs::path> ListWalletDir()
{
    throw std::logic_error("Wallet function called in non-wallet build.");
}

std::vector<std::shared_ptr<CWallet>> GetWallets()
{
    throw std::logic_error("Wallet function called in non-wallet build.");
}

namespace interfaces {

class Wallet;

std::unique_ptr<Wallet> MakeWallet(const std::shared_ptr<CWallet>& wallet)
{
    throw std::logic_error("Wallet function called in non-wallet build.");
}

} // namespace interfaces
