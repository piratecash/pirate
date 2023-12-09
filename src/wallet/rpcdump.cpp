// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2014-2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <core_io.h>
#include <interfaces/chain.h>
#include <key_io.h>
#include <merkleblock.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <script/script.h>
#include <script/standard.h>
#include <sync.h>
#include <util/system.h>
#include <util/time.h>
#include <validation.h>
#include <wallet/wallet.h>

#include <wallet/rpcwallet.h>

#include <stdint.h>

#include <boost/algorithm/string.hpp>

#include <univalue.h>

std::string static EncodeDumpString(const std::string &str) {
    std::stringstream ret;
    for (const unsigned char c : str) {
        if (c <= 32 || c >= 128 || c == '%') {
            ret << '%' << HexStr(Span<const unsigned char>(&c, 1));
        } else {
            ret << c;
        }
    }
    return ret.str();
}

static std::string DecodeDumpString(const std::string &str) {
    std::stringstream ret;
    for (unsigned int pos = 0; pos < str.length(); pos++) {
        unsigned char c = str[pos];
        if (c == '%' && pos+2 < str.length()) {
            c = (((str[pos+1]>>6)*9+((str[pos+1]-'0')&15)) << 4) |
                ((str[pos+2]>>6)*9+((str[pos+2]-'0')&15));
            pos += 2;
        }
        ret << c;
    }
    return ret.str();
}

UniValue importprivkey(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw std::runtime_error(
            RPCHelpMan{"importprivkey",
                "\nAdds a private key (as returned by dumpprivkey) to your wallet. Requires a new wallet backup.\n"
                "Hint: use importmulti to import more than one private key.\n",
                {
                    {"privkey", RPCArg::Type::STR, /* opt */ false, /* default_val */ "", "The private key (see dumpprivkey)"},
                    {"label", RPCArg::Type::STR, /* opt */ true, /* default_val */ "", "An optional label"},
                    {"rescan", RPCArg::Type::BOOL, /* opt */ true, /* default_val */ "true", "Rescan the wallet for transactions"},
                }}
                .ToString() +
            "\nNote: This call can take over an hour to complete if rescan is true, during that time, other rpc calls\n"
            "may report that the imported key exists but related transactions are still missing, leading to temporarily incorrect/bogus balances and unspent outputs until rescan completes.\n"
            "\nExamples:\n"
            "\nDump a private key\n"
            + HelpExampleCli("dumpprivkey", "\"myaddress\"") +
            "\nImport the private key with rescan\n"
            + HelpExampleCli("importprivkey", "\"mykey\"") +
            "\nImport using a label and without rescan\n"
            + HelpExampleCli("importprivkey", "\"mykey\" \"testing\" false") +
            "\nImport using default blank label and without rescan\n"
            + HelpExampleCli("importprivkey", "\"mykey\" \"\" false") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("importprivkey", "\"mykey\", \"testing\", false")
        );

    if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Cannot import private keys to a wallet with private keys disabled");
    }

    WalletRescanReserver reserver(pwallet);
    bool fRescan = true;
    {
        auto locked_chain = pwallet->chain().lock();
        LOCK(pwallet->cs_wallet);

        EnsureWalletIsUnlocked(pwallet);

        std::string strSecret = request.params[0].get_str();
        std::string strLabel = "";
        if (!request.params[1].isNull())
            strLabel = request.params[1].get_str();

        // Whether to perform rescan after import
        if (!request.params[2].isNull())
            fRescan = request.params[2].get_bool();

        if (fRescan && fPruneMode)
            throw JSONRPCError(RPC_WALLET_ERROR, "Rescan is disabled in pruned mode");

        if (fRescan && !reserver.reserve()) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
        }

        CKey key = DecodeSecret(strSecret);
        if (!key.IsValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key encoding");

        CPubKey pubkey = key.GetPubKey();
        assert(key.VerifyPubKey(pubkey));
        CKeyID vchAddress = pubkey.GetID();
        {
            pwallet->MarkDirty();
            pwallet->SetAddressBook(vchAddress, strLabel, "receive");

            // Don't throw error in case a key is already there
            if (pwallet->HaveKey(vchAddress)) {
                return NullUniValue;
            }

            // whenever a key is imported, we need to scan the whole chain
            pwallet->UpdateTimeFirstKey(1);
            pwallet->mapKeyMetadata[vchAddress].nCreateTime = 1;

            if (!pwallet->AddKeyPubKey(key, pubkey)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Error adding key to wallet");
            }
        }
    }
    if (fRescan) {
        int64_t scanned_time = pwallet->RescanFromTime(TIMESTAMP_MIN, reserver, true /* update */);
        if (pwallet->IsAbortingRescan()) {
            throw JSONRPCError(RPC_MISC_ERROR, "Rescan aborted by user.");
        }
        if (scanned_time > TIMESTAMP_MIN) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Rescan was unable to fully rescan the blockchain. Some transactions may be missing.");
        }
    }
    return NullUniValue;
}

UniValue abortrescan(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 0)
        throw std::runtime_error(
            RPCHelpMan{"abortrescan",
                "\nStops current wallet rescan triggered by an RPC call, e.g. by an importprivkey call.\n", {}}
                .ToString() +
            "\nExamples:\n"
            "\nImport a private key\n"
            + HelpExampleCli("importprivkey", "\"mykey\"") +
            "\nAbort the running wallet rescan\n"
            + HelpExampleCli("abortrescan", "") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("abortrescan", "")
        );

    if (!pwallet->IsScanning() || pwallet->IsAbortingRescan()) return false;
    pwallet->AbortRescan();
    return true;
}

static void ImportAddress(CWallet*, const CTxDestination& dest, const std::string& strLabel);
static void ImportScript(CWallet * const pwallet, const CScript& script, const std::string& strLabel, bool isRedeemScript) EXCLUSIVE_LOCKS_REQUIRED(pwallet->cs_wallet)
{
    if (!isRedeemScript && ::IsMine(*pwallet, script) == ISMINE_SPENDABLE) {
        throw JSONRPCError(RPC_WALLET_ERROR, "The wallet already contains the private key for this address or script");
    }

    pwallet->MarkDirty();

    if (!pwallet->HaveWatchOnly(script) && !pwallet->AddWatchOnly(script, 0 /* nCreateTime */)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error adding address to wallet");
    }

    if (isRedeemScript) {
        const CScriptID id(script);
        if (!pwallet->HaveCScript(id) && !pwallet->AddCScript(script)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding p2sh redeemScript to wallet");
        }
        ImportAddress(pwallet, id, strLabel);
    } else {
        CTxDestination destination;
        if (ExtractDestination(script, destination)) {
            pwallet->SetAddressBook(destination, strLabel, "receive");
        }
    }
}

static void ImportAddress(CWallet * const pwallet, const CTxDestination& dest, const std::string& strLabel) EXCLUSIVE_LOCKS_REQUIRED(pwallet->cs_wallet)
{
    CScript script = GetScriptForDestination(dest);
    ImportScript(pwallet, script, strLabel, false);
    // add to address book or update label
    if (IsValidDestination(dest))
        pwallet->SetAddressBook(dest, strLabel, "receive");
}

UniValue importaddress(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 4)
        throw std::runtime_error(
            RPCHelpMan{"importaddress",
                "\nAdds an address or script (in hex) that can be watched as if it were in your wallet but cannot be used to spend. Requires a new wallet backup.\n",
                {
                    {"address", RPCArg::Type::STR, /* opt */ false, /* default_val */ "", "The Dash address (or hex-encoded script)"},
                    {"label", RPCArg::Type::STR, /* opt */ true, /* default_val */ "\"\"", "An optional label"},
                    {"rescan", RPCArg::Type::BOOL, /* opt */ true, /* default_val */ "true", "Rescan the wallet for transactions"},
                    {"p2sh", RPCArg::Type::BOOL, /* opt */ true, /* default_val */ "false", "Add the P2SH version of the script as well"},
                }}
                .ToString() +
            "\nNote: This call can take over an hour to complete if rescan is true, during that time, other rpc calls\n"
            "may report that the imported address exists but related transactions are still missing, leading to temporarily incorrect/bogus balances and unspent outputs until rescan completes.\n"
            "If you have the full public key, you should call importpubkey instead of this.\n"
            "\nNote: If you import a non-standard raw script in hex form, outputs sending to it will be treated\n"
            "as change, and not show up in many RPCs.\n"
            "\nExamples:\n"
            "\nImport an address with rescan\n"
            + HelpExampleCli("importaddress", "\"myaddress\"") +
            "\nImport using a label without rescan\n"
            + HelpExampleCli("importaddress", "\"myaddress\" \"testing\" false") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("importaddress", "\"myaddress\", \"testing\", false")
        );


    std::string strLabel;
    if (!request.params[1].isNull())
        strLabel = request.params[1].get_str();

    // Whether to perform rescan after import
    bool fRescan = true;
    if (!request.params[2].isNull())
        fRescan = request.params[2].get_bool();

    if (fRescan && fPruneMode)
        throw JSONRPCError(RPC_WALLET_ERROR, "Rescan is disabled in pruned mode");

    WalletRescanReserver reserver(pwallet);
    if (fRescan && !reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    // Whether to import a p2sh version, too
    bool fP2SH = false;
    if (!request.params[3].isNull())
        fP2SH = request.params[3].get_bool();

    {
        auto locked_chain = pwallet->chain().lock();
        LOCK(pwallet->cs_wallet);

        CTxDestination dest = DecodeDestination(request.params[0].get_str());
        if (IsValidDestination(dest)) {
            if (fP2SH) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Cannot use the p2sh flag with an address - use a script instead");
            }
            ImportAddress(pwallet, dest, strLabel);
        } else if (IsHex(request.params[0].get_str())) {
            std::vector<unsigned char> data(ParseHex(request.params[0].get_str()));
            ImportScript(pwallet, CScript(data.begin(), data.end()), strLabel, fP2SH);
        } else {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid PirateCash address or script");
        }
    }
    if (fRescan)
    {
        int64_t scanned_time = pwallet->RescanFromTime(TIMESTAMP_MIN, reserver, true /* update */);
        if (pwallet->IsAbortingRescan()) {
            throw JSONRPCError(RPC_MISC_ERROR, "Rescan aborted by user.");
        }
        if (scanned_time > TIMESTAMP_MIN) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Rescan was unable to fully rescan the blockchain. Some transactions may be missing.");
        }
        pwallet->ReacceptWalletTransactions();
    }

    return NullUniValue;
}

UniValue importprunedfunds(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            RPCHelpMan{"importprunedfunds",
                "\nImports funds without rescan. Corresponding address or script must previously be included in wallet. Aimed towards pruned wallets. The end-user is responsible to import additional transactions that subsequently spend the imported outputs or rescan after the point in the blockchain the transaction is included.\n",
                {
                    {"rawtransaction", RPCArg::Type::STR_HEX, /* opt */ false, /* default_val */ "", "A raw transaction in hex funding an already-existing address in wallet"},
                    {"txoutproof", RPCArg::Type::STR_HEX, /* opt */ false, /* default_val */ "", "The hex output from gettxoutproof that contains the transaction"},
                }}
                .ToString()
        );

    CMutableTransaction tx;
    if (!DecodeHexTx(tx, request.params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    uint256 hashTx = tx.GetHash();
    CWalletTx wtx(pwallet, MakeTransactionRef(std::move(tx)));

    CDataStream ssMB(ParseHexV(request.params[1], "proof"), SER_NETWORK, PROTOCOL_VERSION);
    CMerkleBlock merkleBlock;
    ssMB >> merkleBlock;

    //Search partial merkle tree in proof for our transaction and index in valid block
    std::vector<uint256> vMatch;
    std::vector<unsigned int> vIndex;
    unsigned int txnIndex = 0;
    if (merkleBlock.txn.ExtractMatches(vMatch, vIndex) == merkleBlock.header.hashMerkleRoot) {

        auto locked_chain = pwallet->chain().lock();
        const CBlockIndex* pindex = LookupBlockIndex(merkleBlock.header.GetHash());
        if (!pindex || !chainActive.Contains(pindex)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found in chain");
        }

        std::vector<uint256>::const_iterator it;
        if ((it = std::find(vMatch.begin(), vMatch.end(), hashTx))==vMatch.end()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction given doesn't exist in proof");
        }

        txnIndex = vIndex[it - vMatch.begin()];
    }
    else {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Something wrong with merkleblock");
    }

    wtx.nIndex = txnIndex;
    wtx.hashBlock = merkleBlock.header.GetHash();

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    if (pwallet->IsMine(*wtx.tx)) {
        pwallet->AddToWallet(wtx, false);
        return NullUniValue;
    }

    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No addresses in wallet correspond to included transaction");
}

UniValue removeprunedfunds(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            RPCHelpMan{"removeprunedfunds",
                "\nDeletes the specified transaction from the wallet. Meant for use with pruned wallets and as a companion to importprunedfunds. This will affect wallet balances.\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, /* opt */ false, /* default_val */ "", "The hex-encoded id of the transaction you are deleting"},
                }}
                .ToString() +
            "\nExamples:\n"
            + HelpExampleCli("removeprunedfunds", "\"a8d0c0184dde994a09ec054286f1ce581bebf46446a512166eae7628734ea0a5\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("removeprunedfunds", "\"a8d0c0184dde994a09ec054286f1ce581bebf46446a512166eae7628734ea0a5\"")
        );

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    uint256 hash;
    hash.SetHex(request.params[0].get_str());
    std::vector<uint256> vHash;
    vHash.push_back(hash);
    std::vector<uint256> vHashOut;

    if (pwallet->ZapSelectTx(vHash, vHashOut) != DBErrors::LOAD_OK) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Could not properly delete the transaction.");
    }

    if(vHashOut.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Transaction does not exist in wallet.");
    }

    return NullUniValue;
}

UniValue importpubkey(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 4)
        throw std::runtime_error(
            RPCHelpMan{"importpubkey",
                "\nAdds a public key (in hex) that can be watched as if it were in your wallet but cannot be used to spend. Requires a new wallet backup.\n",
                {
                    {"pubkey", RPCArg::Type::STR, /* opt */ false, /* default_val */ "", "The hex-encoded public key"},
                    {"label", RPCArg::Type::STR, /* opt */ true, /* default_val */ "\"\"", "An optional label"},
                    {"rescan", RPCArg::Type::BOOL, /* opt */ true, /* default_val */ "true", "Rescan the wallet for transactions"},
                }}
                .ToString() +
            "\nNote: This call can take over an hour to complete if rescan is true, during that time, other rpc calls\n"
            "may report that the imported pubkey exists but related transactions are still missing, leading to temporarily incorrect/bogus balances and unspent outputs until rescan completes.\n"
            "\nExamples:\n"
            "\nImport a public key with rescan\n"
            + HelpExampleCli("importpubkey", "\"mypubkey\"") +
            "\nImport using a label without rescan\n"
            + HelpExampleCli("importpubkey", "\"mypubkey\" \"testing\" false") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("importpubkey", "\"mypubkey\", \"testing\", false")
        );


    std::string strLabel;
    if (!request.params[1].isNull())
        strLabel = request.params[1].get_str();

    // Whether to perform rescan after import
    bool fRescan = true;
    if (!request.params[2].isNull())
        fRescan = request.params[2].get_bool();

    if (fRescan && fPruneMode)
        throw JSONRPCError(RPC_WALLET_ERROR, "Rescan is disabled in pruned mode");

    WalletRescanReserver reserver(pwallet);
    if (fRescan && !reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    if (!IsHex(request.params[0].get_str()))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Pubkey must be a hex string");
    std::vector<unsigned char> data(ParseHex(request.params[0].get_str()));
    CPubKey pubKey(data.begin(), data.end());
    if (!pubKey.IsFullyValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Pubkey is not a valid public key");

    {
        auto locked_chain = pwallet->chain().lock();
        LOCK(pwallet->cs_wallet);

        ImportAddress(pwallet, pubKey.GetID(), strLabel);
        ImportScript(pwallet, GetScriptForRawPubKey(pubKey), strLabel, false);
    }
    if (fRescan)
    {
        int64_t scanned_time = pwallet->RescanFromTime(TIMESTAMP_MIN, reserver, true /* update */);
        if (pwallet->IsAbortingRescan()) {
            throw JSONRPCError(RPC_MISC_ERROR, "Rescan aborted by user.");
        }
        if (scanned_time > TIMESTAMP_MIN) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Rescan was unable to fully rescan the blockchain. Some transactions may be missing.");
        }
        pwallet->ReacceptWalletTransactions();
    }

    return NullUniValue;
}


UniValue importwallet(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            RPCHelpMan{"importwallet",
                "\nImports keys from a wallet dump file (see dumpwallet). Requires a new wallet backup to include imported keys.\n",
                {
                    {"filename", RPCArg::Type::STR, /* opt */ false, /* default_val */ "", "The wallet file"},
                }}
                .ToString() +
            "\nExamples:\n"
            "\nDump the wallet\n"
            + HelpExampleCli("dumpwallet", "\"test\"") +
            "\nImport the wallet\n"
            + HelpExampleCli("importwallet", "\"test\"") +
            "\nImport using the json rpc call\n"
            + HelpExampleRpc("importwallet", "\"test\"")
        );

    if (fPruneMode)
        throw JSONRPCError(RPC_WALLET_ERROR, "Importing wallets is disabled in pruned mode");

    WalletRescanReserver reserver(pwallet);
    if (!reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    int64_t nTimeBegin = 0;
    bool fGood = true;
    {
        auto locked_chain = pwallet->chain().lock();
        LOCK(pwallet->cs_wallet);

        EnsureWalletIsUnlocked(pwallet);

        fsbridge::ifstream file;
        file.open(request.params[0].get_str(), std::ios::in | std::ios::ate);
        if (!file.is_open()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot open wallet dump file");
        }
        nTimeBegin = chainActive.Tip()->GetBlockTime();

        int64_t nFilesize = std::max((int64_t)1, (int64_t)file.tellg());
        file.seekg(0, file.beg);

        // Use uiInterface.ShowProgress instead of pwallet.ShowProgress because pwallet.ShowProgress has a cancel button tied to AbortRescan which
        // we don't want for this progress bar showing the import progress. uiInterface.ShowProgress does not have a cancel button.
        uiInterface.ShowProgress(strprintf("%s " + _("Importing..."), pwallet->GetDisplayName()), 0, false); // show progress dialog in GUI
        std::vector<std::tuple<CKey, int64_t, bool, std::string>> keys;
        std::vector<std::pair<CScript, int64_t>> scripts;
        while (file.good()) {
            uiInterface.ShowProgress("", std::max(1, std::min(50, (int)(((double)file.tellg() / (double)nFilesize) * 100))), false);
            std::string line;
            std::getline(file, line);
            if (line.empty() || line[0] == '#')
                continue;

            std::vector<std::string> vstr;
            boost::split(vstr, line, boost::is_any_of(" "));
            if (vstr.size() < 2)
                continue;
            CKey key = DecodeSecret(vstr[0]);
            if (key.IsValid()) {
                int64_t nTime = ParseISO8601DateTime(vstr[1]);
                std::string strLabel;
                bool fLabel = true;
                for (unsigned int nStr = 2; nStr < vstr.size(); nStr++) {
                    if (vstr[nStr].front() == '#')
                        break;
                    if (vstr[nStr] == "change=1")
                        fLabel = false;
                    if (vstr[nStr] == "reserve=1")
                        fLabel = false;
                    if (vstr[nStr].substr(0,6) == "label=") {
                        strLabel = DecodeDumpString(vstr[nStr].substr(6));
                        fLabel = true;
                    }
                }
                keys.push_back(std::make_tuple(key, nTime, fLabel, strLabel));
            } else if(IsHex(vstr[0])) {
                std::vector<unsigned char> vData(ParseHex(vstr[0]));
                CScript script = CScript(vData.begin(), vData.end());
                int64_t birth_time = ParseISO8601DateTime(vstr[1]);
                scripts.push_back(std::pair<CScript, int64_t>(script, birth_time));
            }
        }
        file.close();
        // We now know whether we are importing private keys, so we can error if private keys are disabled
        if (keys.size() > 0 && pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
            uiInterface.ShowProgress("", 100, false); // hide progress dialog in GUI
            throw JSONRPCError(RPC_WALLET_ERROR, "Importing wallets is disabled when private keys are disabled");
        }
        double total = (double)(keys.size() + scripts.size());
        double progress = 0;
        for (const auto& key_tuple : keys) {
            uiInterface.ShowProgress("", std::max(50, std::min(75, (int)((progress / total) * 100) + 50)), false);
            const CKey& key = std::get<0>(key_tuple);
            int64_t time = std::get<1>(key_tuple);
            bool has_label = std::get<2>(key_tuple);
            std::string label = std::get<3>(key_tuple);

            CPubKey pubkey = key.GetPubKey();
            assert(key.VerifyPubKey(pubkey));
            CKeyID keyid = pubkey.GetID();
            if (pwallet->HaveKey(keyid)) {
                pwallet->WalletLogPrintf("Skipping import of %s (key already present)\n", EncodeDestination(keyid));
                continue;
            }
            pwallet->WalletLogPrintf("Importing %s...\n", EncodeDestination(keyid));
            if (!pwallet->AddKeyPubKey(key, pubkey)) {
                fGood = false;
                continue;
            }
            pwallet->mapKeyMetadata[keyid].nCreateTime = time;
            if (has_label)
                pwallet->SetAddressBook(keyid, label, "receive");
            nTimeBegin = std::min(nTimeBegin, time);
            progress++;
        }
        for (const auto& script_pair : scripts) {
            uiInterface.ShowProgress("", std::max(50, std::min(75, (int)((progress / total) * 100) + 50)), false);
            const CScript& script = script_pair.first;
            int64_t time = script_pair.second;
            CScriptID id(script);
            if (pwallet->HaveCScript(id)) {
                pwallet->WalletLogPrintf("Skipping import of %s (script already present)\n", HexStr(script));
                continue;
            }
            if(!pwallet->AddCScript(script)) {
                pwallet->WalletLogPrintf("Error importing script %s\n", HexStr(script));
                fGood = false;
                continue;
            }
            if (time > 0) {
                pwallet->m_script_metadata[id].nCreateTime = time;
                nTimeBegin = std::min(nTimeBegin, time);
            }
            progress++;
        }
        uiInterface.ShowProgress("", 100, false); // hide progress dialog in GUI
        pwallet->UpdateTimeFirstKey(nTimeBegin);
    }
    uiInterface.ShowProgress("", 100, false); // hide progress dialog in GUI
    int64_t scanned_time = pwallet->RescanFromTime(nTimeBegin, reserver, false /* update */);
    if (pwallet->IsAbortingRescan()) {
        throw JSONRPCError(RPC_MISC_ERROR, "Rescan aborted by user.");
    }
    if (scanned_time > nTimeBegin) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Rescan was unable to fully rescan the blockchain. Some transactions may be missing.");
    }
    pwallet->MarkDirty();

    if (!fGood)
        throw JSONRPCError(RPC_WALLET_ERROR, "Error adding some keys/scripts to wallet");

    return NullUniValue;
}

UniValue importelectrumwallet(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            RPCHelpMan{"importselectrumwallet",
                "\nImports keys from an Electrum wallet export file (.csv or .json)\n",
                {
                    {"filename", RPCArg::Type::STR, /* opt */ false, /* default_val */ "", "The Electrum wallet export file, should be in csv or json format"},
                    {"index", RPCArg::Type::NUM, /* opt */ true, /* default_val */ "0", "Rescan the wallet for transactions starting from this block index"},
                }}
                .ToString() +
            "\nExamples:\n"
            "\nImport the wallet\n"
            + HelpExampleCli("importelectrumwallet", "\"test.csv\"")
            + HelpExampleCli("importelectrumwallet", "\"test.json\"") +
            "\nImport using the json rpc call\n"
            + HelpExampleRpc("importelectrumwallet", "\"test.csv\"")
            + HelpExampleRpc("importelectrumwallet", "\"test.json\"")
        );

    if (fPruneMode)
        throw JSONRPCError(RPC_WALLET_ERROR, "Importing wallets is disabled in pruned mode");

    if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Private keys are disabled for this wallet");
    }

    LOCK2(cs_main, pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    fsbridge::ifstream file;
    std::string strFileName = request.params[0].get_str();
    size_t nDotPos = strFileName.find_last_of(".");
    if(nDotPos == std::string::npos)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "File has no extension, should be .json or .csv");

    std::string strFileExt = strFileName.substr(nDotPos+1);
    if(strFileExt != "json" && strFileExt != "csv")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "File has wrong extension, should be .json or .csv");

    file.open(strFileName, std::ios::in | std::ios::ate);
    if (!file.is_open())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot open Electrum wallet export file");

    bool fGood = true;

    int64_t nFilesize = std::max((int64_t)1, (int64_t)file.tellg());
    file.seekg(0, file.beg);

    pwallet->ShowProgress(_("Importing..."), 0); // show progress dialog in GUI

    if(strFileExt == "csv") {
        while (file.good()) {
            pwallet->ShowProgress("", std::max(1, std::min(99, (int)(((double)file.tellg() / (double)nFilesize) * 100))));
            std::string line;
            std::getline(file, line);
            if (line.empty() || line == "address,private_key")
                continue;
            std::vector<std::string> vstr;
            boost::split(vstr, line, boost::is_any_of(","));
            if (vstr.size() < 2)
                continue;
            CKey key = DecodeSecret(vstr[1]);
            if (!key.IsValid()) {
                continue;
            }
            CPubKey pubkey = key.GetPubKey();
            assert(key.VerifyPubKey(pubkey));
            CKeyID keyid = pubkey.GetID();
            if (pwallet->HaveKey(keyid)) {
                pwallet->WalletLogPrintf("Skipping import of %s (key already present)\n", EncodeDestination(keyid));
                continue;
            }
            pwallet->WalletLogPrintf("Importing %s...\n", EncodeDestination(keyid));
            if (!pwallet->AddKeyPubKey(key, pubkey)) {
                fGood = false;
                continue;
            }
        }
    } else {
        // json
        char* buffer = new char [nFilesize];
        file.read(buffer, nFilesize);
        UniValue data(UniValue::VOBJ);
        if(!data.read(buffer))
            throw JSONRPCError(RPC_TYPE_ERROR, "Cannot parse Electrum wallet export file");
        delete[] buffer;

        std::vector<std::string> vKeys = data.getKeys();

        for (size_t i = 0; i < data.size(); i++) {
            pwallet->ShowProgress("", std::max(1, std::min(99, int(i*100/data.size()))));
            if(!data[vKeys[i]].isStr())
                continue;
            CKey key = DecodeSecret(data[vKeys[i]].get_str());
            if (!key.IsValid()) {
                continue;
            }
            CPubKey pubkey = key.GetPubKey();
            assert(key.VerifyPubKey(pubkey));
            CKeyID keyid = pubkey.GetID();
            if (pwallet->HaveKey(keyid)) {
                pwallet->WalletLogPrintf("Skipping import of %s (key already present)\n", EncodeDestination(keyid));
                continue;
            }
            pwallet->WalletLogPrintf("Importing %s...\n", EncodeDestination(keyid));
            if (!pwallet->AddKeyPubKey(key, pubkey)) {
                fGood = false;
                continue;
            }
        }
    }
    file.close();
    pwallet->ShowProgress("", 100); // hide progress dialog in GUI

    // Whether to perform rescan after import
    int nStartHeight = 0;
    if (!request.params[1].isNull())
        nStartHeight = request.params[1].get_int();
    if (chainActive.Height() < nStartHeight)
        nStartHeight = chainActive.Height();

    // Assume that electrum wallet was created at that block
    int nTimeBegin = chainActive[nStartHeight]->GetBlockTime();
    pwallet->UpdateTimeFirstKey(nTimeBegin);

    pwallet->WalletLogPrintf("Rescanning %i blocks\n", chainActive.Height() - nStartHeight + 1);
    WalletRescanReserver reserver(pwallet);
    if (!reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }
    const CBlockIndex *stop_block, *failed_block;
    pwallet->ScanForWalletTransactions(chainActive[nStartHeight], nullptr, reserver, failed_block, stop_block, true);

    if (!fGood)
        throw JSONRPCError(RPC_WALLET_ERROR, "Error adding some keys to wallet");

    return NullUniValue;
}

UniValue dumpprivkey(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            RPCHelpMan{"dumpprivkey",
                "\nReveals the private key corresponding to 'address'.\n"
                "Then the importprivkey can be used with this output\n",
                {
                    {"address", RPCArg::Type::STR, /* opt */ false, /* default_val */ "", "The dash address for the private key"},
                }}
                .ToString() +
            "\nResult:\n"
            "\"key\"                (string) The private key\n"
            "\nExamples:\n"
            + HelpExampleCli("dumpprivkey", "\"myaddress\"")
            + HelpExampleCli("importprivkey", "\"mykey\"")
            + HelpExampleRpc("dumpprivkey", "\"myaddress\"")
        );

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    std::string strAddress = request.params[0].get_str();
    CTxDestination dest = DecodeDestination(strAddress);
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid PirateCash address");
    }
    const CKeyID *keyID = boost::get<CKeyID>(&dest);
    if (!keyID) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to a key");
    }
    CKey vchSecret;
    if (!pwallet->GetKey(*keyID, vchSecret)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key for address " + strAddress + " is not known");
    }
    return EncodeSecret(vchSecret);
}

UniValue dumphdinfo(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            RPCHelpMan{"dumphdinfo",
                "Returns an object containing sensitive private info about this HD wallet.\n",
                {}}
                .ToString() +
            "\nResult:\n"
            "{\n"
            "  \"hdseed\": \"seed\",                    (string) The HD seed (bip32, in hex)\n"
            "  \"mnemonic\": \"words\",                 (string) The mnemonic for this HD wallet (bip39, english words) \n"
            "  \"mnemonicpassphrase\": \"passphrase\",  (string) The mnemonic passphrase for this HD wallet (bip39)\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("dumphdinfo", "")
            + HelpExampleRpc("dumphdinfo", "")
        );

    LOCK(pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    CHDChain hdChainCurrent;
    if (!pwallet->GetHDChain(hdChainCurrent))
        throw JSONRPCError(RPC_WALLET_ERROR, "This wallet is not a HD wallet.");

    if (!pwallet->GetDecryptedHDChain(hdChainCurrent))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Cannot decrypt HD seed");

    SecureString ssMnemonic;
    SecureString ssMnemonicPassphrase;
    hdChainCurrent.GetMnemonic(ssMnemonic, ssMnemonicPassphrase);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("hdseed", HexStr(hdChainCurrent.GetSeed()));
    obj.pushKV("mnemonic", ssMnemonic.c_str());
    obj.pushKV("mnemonicpassphrase", ssMnemonicPassphrase.c_str());

    return obj;
}

UniValue dumpwallet(const JSONRPCRequest& request)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
    CWallet* const pwallet = wallet.get();
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            RPCHelpMan{"dumpwallet",
                "\nDumps all wallet keys in a human-readable format to a server-side file. This does not allow overwriting existing files.\n"
                "Imported scripts are included in the dumpfile too, their corresponding addresses will be added automatically by importwallet.\n"
                "Note that if your wallet contains keys which are not derived from your HD seed (e.g. imported keys), these are not covered by\n"
                "only backing up the seed itself, and must be backed up too (e.g. ensure you back up the whole dumpfile).\n",
                {
                    {"filename", RPCArg::Type::STR, /* opt */ false, /* default_val */ "", "The filename with path (either absolute or relative to dashd)"},
                }}
                .ToString() +
            "\nResult:\n"
            "{                           (json object)\n"
            "  \"keys\" : {            (int) The number of keys contained in the wallet dump\n"
            "  \"filename\" : {        (string) The filename with full absolute path\n"
            "  \"warning\" : {         (string) A warning about not sharing the wallet dump with anyone\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("dumpwallet", "\"test\"")
            + HelpExampleRpc("dumpwallet", "\"test\"")
        );

    auto locked_chain = pwallet->chain().lock();
    LOCK(pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    fs::path filepath = request.params[0].get_str();
    filepath = fs::absolute(filepath);

    /* Prevent arbitrary files from being overwritten. There have been reports
     * that users have overwritten wallet files this way:
     * https://github.com/bitcoin/bitcoin/issues/9934
     * It may also avoid other security issues.
     */
    if (fs::exists(filepath)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, filepath.string() + " already exists. If you are sure this is what you want, move it out of the way first");
    }

    fsbridge::ofstream file;
    file.open(filepath);
    if (!file.is_open())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot open wallet dump file");

    std::map<CTxDestination, int64_t> mapKeyBirth;
    const std::map<CKeyID, int64_t>& mapKeyPool = pwallet->GetAllReserveKeys();
    pwallet->GetKeyBirthTimes(*locked_chain, mapKeyBirth);

    std::set<CScriptID> scripts = pwallet->GetCScripts();
    // TODO: include scripts in GetKeyBirthTimes() output instead of separate

    // sort time/key pairs
    std::vector<std::pair<int64_t, CKeyID> > vKeyBirth;
    for (const auto& entry : mapKeyBirth) {
        if (const CKeyID* keyID = boost::get<CKeyID>(&entry.first)) { // set and test
            vKeyBirth.push_back(std::make_pair(entry.second, *keyID));
        }
    }
    mapKeyBirth.clear();
    std::sort(vKeyBirth.begin(), vKeyBirth.end());

    // produce output
    file << strprintf("# Wallet dump created by PirateCash Core %s\n", CLIENT_BUILD);
    file << strprintf("# * Created on %s\n", FormatISO8601DateTime(GetTime()));
    file << strprintf("# * Best block at time of backup was %i (%s),\n", chainActive.Height(), chainActive.Tip()->GetBlockHash().ToString());
    file << strprintf("#   mined on %s\n", FormatISO8601DateTime(chainActive.Tip()->GetBlockTime()));
    file << "\n";

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("cosantacoreversion", CLIENT_BUILD);
    obj.pushKV("lastblockheight", chainActive.Height());
    obj.pushKV("lastblockhash", chainActive.Tip()->GetBlockHash().ToString());
    obj.pushKV("lastblocktime", FormatISO8601DateTime(chainActive.Tip()->GetBlockTime()));

    // add the base58check encoded extended master if the wallet uses HD
    CHDChain hdChainCurrent;
    if (pwallet->GetHDChain(hdChainCurrent))
    {

        if (!pwallet->GetDecryptedHDChain(hdChainCurrent))
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Cannot decrypt HD chain");

        SecureString ssMnemonic;
        SecureString ssMnemonicPassphrase;
        hdChainCurrent.GetMnemonic(ssMnemonic, ssMnemonicPassphrase);
        file << "# mnemonic: " << ssMnemonic << "\n";
        file << "# mnemonic passphrase: " << ssMnemonicPassphrase << "\n\n";

        SecureVector vchSeed = hdChainCurrent.GetSeed();
        file << "# HD seed: " << HexStr(vchSeed) << "\n\n";

        CExtKey masterKey;
        masterKey.SetSeed(&vchSeed[0], vchSeed.size());

        file << "# extended private masterkey: " << EncodeExtKey(masterKey) << "\n";

        CExtPubKey masterPubkey;
        masterPubkey = masterKey.Neuter();

        file << "# extended public masterkey: " << EncodeExtPubKey(masterPubkey) << "\n\n";

        for (size_t i = 0; i < hdChainCurrent.CountAccounts(); ++i)
        {
            CHDAccount acc;
            if(hdChainCurrent.GetAccount(i, acc)) {
                file << "# external chain counter: " << acc.nExternalChainCounter << "\n";
                file << "# internal chain counter: " << acc.nInternalChainCounter << "\n\n";
            } else {
                file << "# WARNING: ACCOUNT " << i << " IS MISSING!" << "\n\n";
            }
        }
        obj.pushKV("hdaccounts", int(hdChainCurrent.CountAccounts()));
    }

    for (std::vector<std::pair<int64_t, CKeyID> >::const_iterator it = vKeyBirth.begin(); it != vKeyBirth.end(); it++) {
        const CKeyID &keyid = it->second;
        std::string strTime = FormatISO8601DateTime(it->first);
        std::string strAddr = EncodeDestination(keyid);
        CKey key;
        if (pwallet->GetKey(keyid, key)) {
            file << strprintf("%s %s ", EncodeSecret(key), strTime);
            if (pwallet->mapAddressBook.count(keyid)) {
                file << strprintf("label=%s", EncodeDumpString(pwallet->mapAddressBook[keyid].name));
            } else if (mapKeyPool.count(keyid)) {
                file << "reserve=1";
            } else {
                file << "change=1";
            }
            file << strprintf(" # addr=%s%s\n", strAddr, (pwallet->mapHdPubKeys.count(keyid) ? " hdkeypath="+pwallet->mapHdPubKeys[keyid].GetKeyPath() : ""));
        }
    }
    file << "\n";
    for (const CScriptID &scriptid : scripts) {
        CScript script;
        std::string create_time = "0";
        std::string address = EncodeDestination(scriptid);
        // get birth times for scripts with metadata
        auto it = pwallet->m_script_metadata.find(scriptid);
        if (it != pwallet->m_script_metadata.end()) {
            create_time = FormatISO8601DateTime(it->second.nCreateTime);
        }
        if(pwallet->GetCScript(scriptid, script)) {
            file << strprintf("%s %s script=1", HexStr(script), create_time);
            file << strprintf(" # addr=%s\n", address);
        }
    }
    file << "\n";
    file << "# End of dump\n";
    file.close();

    std::string strWarning = strprintf(_("%s file contains all private keys from this wallet. Do not share it with anyone!"), request.params[0].get_str().c_str());
    obj.pushKV("keys", int(vKeyBirth.size()));
    obj.pushKV("filename", filepath.string());
    obj.pushKV("warning", strWarning);

    return obj;
}


static UniValue ProcessImport(CWallet * const pwallet, const UniValue& data, const int64_t timestamp) EXCLUSIVE_LOCKS_REQUIRED(pwallet->cs_wallet)
{
    try {
        // First ensure scriptPubKey has either a script or JSON with "address" string
        const UniValue& scriptPubKey = data["scriptPubKey"];
        bool isScript = scriptPubKey.getType() == UniValue::VSTR;
        if (!isScript && !(scriptPubKey.getType() == UniValue::VOBJ && scriptPubKey.exists("address"))) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "scriptPubKey must be string with script or JSON with address string");
        }
        const std::string& output = isScript ? scriptPubKey.get_str() : scriptPubKey["address"].get_str();

        // Optional fields.
        const std::string& strRedeemScript = data.exists("redeemscript") ? data["redeemscript"].get_str() : "";
        const UniValue& pubKeys = data.exists("pubkeys") ? data["pubkeys"].get_array() : UniValue();
        const UniValue& keys = data.exists("keys") ? data["keys"].get_array() : UniValue();
        const bool internal = data.exists("internal") ? data["internal"].get_bool() : false;
        const bool watchOnly = data.exists("watchonly") ? data["watchonly"].get_bool() : false;
        const std::string& label = data.exists("label") ? data["label"].get_str() : "";

        // Parse the output.
        // If private keys are disabled, abort if private keys are being imported
        if (pwallet->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) && !keys.isNull()) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Cannot import private keys to a wallet with private keys disabled");
        }

        // Generate the script and destination for the scriptPubKey provided
        CScript script;
        CTxDestination dest;

        if (!isScript) {
            dest = DecodeDestination(output);
            if (!IsValidDestination(dest)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
            }
            script = GetScriptForDestination(dest);
        } else {
            if (!IsHex(output)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid scriptPubKey");
            }

            std::vector<unsigned char> vData(ParseHex(output));
            script = CScript(vData.begin(), vData.end());
            if (!ExtractDestination(script, dest) && !internal) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Internal must be set to true for nonstandard scriptPubKey imports.");
            }
        }

        // Watchonly and private keys
        if (watchOnly && keys.size()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Watch-only addresses should not include private keys");
        }

        // Internal addresses should not have a label
        if (internal && data.exists("label")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Internal addresses should not have a label");
        }

        CScript scriptpubkey_script = script;
        CTxDestination scriptpubkey_dest = dest;

        // P2SH
        if (!strRedeemScript.empty() && script.IsPayToScriptHash()) {
            // Check the redeemScript is valid
            if (!IsHex(strRedeemScript)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid redeem script: must be hex string");
            }

            // Import redeem script.
            std::vector<unsigned char> vData(ParseHex(strRedeemScript));
            CScript redeemScript = CScript(vData.begin(), vData.end());
            CScriptID redeem_id(redeemScript);

            // Check that the redeemScript and scriptPubKey match
            if (GetScriptForDestination(redeem_id) != script) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "The redeemScript does not match the scriptPubKey");
            }

            pwallet->MarkDirty();

            if (!pwallet->AddWatchOnly(redeemScript, timestamp)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Error adding address to wallet");
            }

            if (!pwallet->HaveCScript(redeem_id) && !pwallet->AddCScript(redeemScript)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Error adding p2sh redeemScript to wallet");
            }

            // Now set script to the redeemScript so we parse the inner script as P2WSH or P2WPKH below
            script = redeemScript;
            ExtractDestination(script, dest);
        }

        // (P2SH-)P2PK/P2PKH
        if (dest.type() == typeid(CKeyID)) {
            if (keys.size() > 1 || pubKeys.size() > 1) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "More than one key given for one single-key address");
            }
            CPubKey pubkey;
            if (keys.size()) {
                pubkey = DecodeSecret(keys[0].get_str()).GetPubKey();
            }
            if (pubKeys.size()) {
                const std::string& strPubKey = pubKeys[0].get_str();
                if (!IsHex(strPubKey)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Pubkey must be a hex string");
                }
                std::vector<unsigned char> vData(ParseHex(pubKeys[0].get_str()));
                CPubKey pubkey_temp(vData.begin(), vData.end());
                if (pubkey.size() && pubkey_temp != pubkey) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Private key does not match public key for address");
                }
                pubkey = pubkey_temp;
            }
            if (pubkey.size() > 0) {
                if (!pubkey.IsFullyValid()) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Pubkey is not a valid public key");
                }

                // Check the key corresponds to the destination given
                CTxDestination pubkey_dest = pubkey.GetID();
                if (!(pubkey_dest == dest)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Key does not match address destination");
                }

                // This is necessary to force the wallet to import the pubKey
                CScript scriptRawPubKey = GetScriptForRawPubKey(pubkey);

                if (::IsMine(*pwallet, scriptRawPubKey) == ISMINE_SPENDABLE) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "The wallet already contains the private key for this address or script");
                }

                pwallet->MarkDirty();

                if (!pwallet->AddWatchOnly(scriptRawPubKey, timestamp)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Error adding address to wallet");
                }
            }
        }

        // Import the address
        if (::IsMine(*pwallet, scriptpubkey_script) == ISMINE_SPENDABLE) {
            throw JSONRPCError(RPC_WALLET_ERROR, "The wallet already contains the private key for this address or script");
        }

        pwallet->MarkDirty();

        if (!pwallet->AddWatchOnly(scriptpubkey_script, timestamp)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding address to wallet");
        }

        if (!watchOnly && !pwallet->HaveCScript(CScriptID(scriptpubkey_script)) && !pwallet->AddCScript(scriptpubkey_script)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding scriptPubKey script to wallet");
        }

        // if not internal add to address book or update label
        if (!internal) {
            assert(IsValidDestination(scriptpubkey_dest));
            pwallet->SetAddressBook(scriptpubkey_dest, label, "receive");
        }

        // Import private keys.
        for (size_t i = 0; i < keys.size(); i++) {
            const std::string& strPrivkey = keys[i].get_str();

            // Checks.
            CKey key = DecodeSecret(strPrivkey);

            if (!key.IsValid()) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key encoding");
            }

            CPubKey pubKey = key.GetPubKey();
            assert(key.VerifyPubKey(pubKey));

            CKeyID vchAddress = pubKey.GetID();
            pwallet->MarkDirty();

            if (pwallet->HaveKey(vchAddress)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Already have this key");
            }

            pwallet->mapKeyMetadata[vchAddress].nCreateTime = timestamp;

            if (!pwallet->AddKeyPubKey(key, pubKey)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Error adding key to wallet");
            }

            pwallet->UpdateTimeFirstKey(timestamp);
        }

        UniValue result = UniValue(UniValue::VOBJ);
        result.pushKV("success", UniValue(true));
        return result;
    } catch (const UniValue& e) {
        UniValue result = UniValue(UniValue::VOBJ);
        result.pushKV("success", UniValue(false));
        result.pushKV("error", e);
        return result;
    } catch (...) {
        UniValue result = UniValue(UniValue::VOBJ);
        result.pushKV("success", UniValue(false));
        result.pushKV("error", JSONRPCError(RPC_MISC_ERROR, "Missing required fields"));
        return result;
    }
}

static int64_t GetImportTimestamp(const UniValue& data, int64_t now)
{
    if (data.exists("timestamp")) {
        const UniValue& timestamp = data["timestamp"];
        if (timestamp.isNum()) {
            return timestamp.get_int64();
        } else if (timestamp.isStr() && timestamp.get_str() == "now") {
            return now;
        }
        throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Expected number or \"now\" timestamp value for key. got type %s", uvTypeName(timestamp.type())));
    }
    throw JSONRPCError(RPC_TYPE_ERROR, "Missing required timestamp field for key");
}

UniValue importmulti(const JSONRPCRequest& mainRequest)
{
    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(mainRequest);
    CWallet* const pwallet = wallet.get();
    if (!EnsureWalletIsAvailable(pwallet, mainRequest.fHelp)) {
        return NullUniValue;
    }

    if (mainRequest.fHelp || mainRequest.params.size() < 1 || mainRequest.params.size() > 2)
        throw std::runtime_error(
            RPCHelpMan{"importmulti",
                "\nImport addresses/scripts (with private or public keys, redeem script (P2SH)), rescanning all addresses in one-shot-only (rescan can be disabled via options). Requires a new wallet backup.\n",
                {
                    {"requests", RPCArg::Type::ARR, /* opt */ false, /* default_val */ "", "Data to be imported",
                        {
                            {"", RPCArg::Type::OBJ, /* opt */ false, /* default_val */ "", "",
                                {
                                    {"scriptPubKey", RPCArg::Type::STR, /* opt */ false, /* default_val */ "", "Type of scriptPubKey (string for script, json for address)",
                                        /* oneline_description */ "", {"\"<script>\" | { \"address\":\"<address>\" }", "string / json"}
                                    },
                                    {"timestamp", RPCArg::Type::NUM, /* opt */ false, /* default_val */ "", "Creation time of the key in seconds since epoch (Jan 1 1970 GMT),\n"
        "                                                              or the string \"now\" to substitute the current synced blockchain time. The timestamp of the oldest\n"
        "                                                              key will determine how far back blockchain rescans need to begin for missing wallet transactions.\n"
        "                                                              \"now\" can be specified to bypass scanning, for keys which are known to never have been used, and\n"
        "                                                              0 can be specified to scan the entire blockchain. Blocks up to 2 hours before the earliest key\n"
        "                                                              creation time of all keys being imported by the importmulti call will be scanned.",
                                        /* oneline_description */ "", {"timestamp | \"now\"", "integer / string"}
                                    },
                                    {"redeemscript", RPCArg::Type::STR, /* opt */ true, /* default_val */ "", "Allowed only if the scriptPubKey is a P2SH address or a P2SH scriptPubKey"},
                                    {"pubkeys", RPCArg::Type::ARR, /* opt */ true, /* default_val */ "", "Array of strings giving pubkeys that must occur in the output or redeemscript",
                                        {
                                            {"pubKey", RPCArg::Type::STR, /* opt */ false, /* default_val */ "", ""},
                                        }
                                    },
                                    {"keys", RPCArg::Type::ARR, /* opt */ true, /* default_val */ "", "Array of strings giving private keys whose corresponding public keys must occur in the output or redeemscript",
                                        {
                                            {"key", RPCArg::Type::STR, /* opt */ false, /* default_val */ "", ""},
                                        }
                                    },
                                    {"internal", RPCArg::Type::BOOL, /* opt */ true, /* default_val */ "false", "Stating whether matching outputs should be treated as not incoming payments aka change"},
                                    {"watchonly", RPCArg::Type::BOOL, /* opt */ true, /* default_val */ "false", "Stating whether matching outputs should be considered watched even when they're not spendable, only allowed if keys are empty"},
                                    {"label", RPCArg::Type::STR, /* opt */ true, /* default_val */ "''", "Label to assign to the address, only allowed with internal=false"},
                                },
                            },
                        },
                        "\"requests\""},
                    {"options", RPCArg::Type::OBJ, /* opt */ true, /* default_val */ "", "",
                        {
                            {"rescan", RPCArg::Type::BOOL, /* opt */ true, /* default_val */ "true", "Stating if should rescan the blockchain after all imports"},
                        },
                        "\"options\""},
                }}
                .ToString() +
            "\nNote: This call can take over an hour to complete if rescan is true, during that time, other rpc calls\n"
            "may report that the imported keys, addresses or scripts exists but related transactions are still missing.\n"
            "\nExamples:\n" +
            HelpExampleCli("importmulti", "'[{ \"scriptPubKey\": { \"address\": \"<my address>\" }, \"timestamp\":1455191478 }, "
                                          "{ \"scriptPubKey\": { \"address\": \"<my 2nd address>\" }, \"label\": \"example 2\", \"timestamp\": 1455191480 }]'") +
            HelpExampleCli("importmulti", "'[{ \"scriptPubKey\": { \"address\": \"<my address>\" }, \"timestamp\":1455191478 }]' '{ \"rescan\": false}'") +

            "\nResponse is an array with the same size as the input that has the execution result :\n"
            "  [{ \"success\": true } , { \"success\": false, \"error\": { \"code\": -1, \"message\": \"Internal Server Error\"} }, ... ]\n");


    RPCTypeCheck(mainRequest.params, {UniValue::VARR, UniValue::VOBJ});

    const UniValue& requests = mainRequest.params[0];

    //Default options
    bool fRescan = true;

    if (!mainRequest.params[1].isNull()) {
        const UniValue& options = mainRequest.params[1];

        if (options.exists("rescan")) {
            fRescan = options["rescan"].get_bool();
        }
    }

    WalletRescanReserver reserver(pwallet);
    if (fRescan && !reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    int64_t now = 0;
    bool fRunScan = false;
    int64_t nLowestTimestamp = 0;
    UniValue response(UniValue::VARR);
    {
        auto locked_chain = pwallet->chain().lock();
        LOCK(pwallet->cs_wallet);
        EnsureWalletIsUnlocked(pwallet);

        // Verify all timestamps are present before importing any keys.
        now = chainActive.Tip() ? chainActive.Tip()->GetMedianTimePast() : 0;
        for (const UniValue& data : requests.getValues()) {
            GetImportTimestamp(data, now);
        }

        const int64_t minimumTimestamp = 1;

        if (fRescan && chainActive.Tip()) {
            nLowestTimestamp = chainActive.Tip()->GetBlockTime();
        } else {
            fRescan = false;
        }

        for (const UniValue& data : requests.getValues()) {
            const int64_t timestamp = std::max(GetImportTimestamp(data, now), minimumTimestamp);
            const UniValue result = ProcessImport(pwallet, data, timestamp);
            response.push_back(result);

            if (!fRescan) {
                continue;
            }

            // If at least one request was successful then allow rescan.
            if (result["success"].get_bool()) {
                fRunScan = true;
            }

            // Get the lowest timestamp.
            if (timestamp < nLowestTimestamp) {
                nLowestTimestamp = timestamp;
            }
        }
    }
    if (fRescan && fRunScan && requests.size()) {
        int64_t scannedTime = pwallet->RescanFromTime(nLowestTimestamp, reserver, true /* update */);
        pwallet->ReacceptWalletTransactions();

        if (pwallet->IsAbortingRescan()) {
            throw JSONRPCError(RPC_MISC_ERROR, "Rescan aborted by user.");
        }
        if (scannedTime > nLowestTimestamp) {
            std::vector<UniValue> results = response.getValues();
            response.clear();
            response.setArray();
            size_t i = 0;
            for (const UniValue& request : requests.getValues()) {
                // If key creation date is within the successfully scanned
                // range, or if the import result already has an error set, let
                // the result stand unmodified. Otherwise replace the result
                // with an error message.
                if (scannedTime <= GetImportTimestamp(request, now) || results.at(i).exists("error")) {
                    response.push_back(results.at(i));
                } else {
                    UniValue result = UniValue(UniValue::VOBJ);
                    result.pushKV("success", UniValue(false));
                    result.pushKV(
                        "error",
                        JSONRPCError(
                            RPC_MISC_ERROR,
                            strprintf("Rescan failed for key with creation timestamp %d. There was an error reading a "
                                      "block from time %d, which is after or within %d seconds of key creation, and "
                                      "could contain transactions pertaining to the key. As a result, transactions "
                                      "and coins using this key may not appear in the wallet. This error could be "
                                      "caused by pruning or data corruption (see cosantad log for details) and could "
                                      "be dealt with by downloading and rescanning the relevant blocks (see -reindex "
                                      "and -rescan options).",
                                GetImportTimestamp(request, now), scannedTime - TIMESTAMP_WINDOW - 1, TIMESTAMP_WINDOW)));
                    response.push_back(std::move(result));
                }
                ++i;
            }
        }
    }

    return response;
}
