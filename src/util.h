// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Copyright (c) 2014-2019 The Dash Core developers
// Copyright (c) 2020-2022 The Cosanta Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/**
 * Server/client environment: argument handling, config file parsing,
 * thread wrappers, startup time
 */
#ifndef BITCOIN_UTIL_H
#define BITCOIN_UTIL_H

#if defined(HAVE_CONFIG_H)
#include <config/cosanta-config.h>
#endif

#include <attributes.h>
#include <compat.h>
#include <fs.h>
#include <logging.h>
#include <sync.h>
#include <tinyformat.h>
#include <utiltime.h>
#include <utilmemory.h>
#include <amount.h>

#include <atomic>
#include <exception>
#include <map>
#include <set>
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/signals2/signal.hpp>
#include <boost/thread/condition_variable.hpp> // for boost::thread_interrupted

// Debugging macros

// Uncomment the following line to enable debugging messages
// or enable on a per file basis prior to inclusion of util.h
//#define ENABLE_COSANTA_DEBUG
#ifdef ENABLE_COSANTA_DEBUG
#define DBG( x ) x
#else
#define DBG( x )
#endif

//Cosanta only features

extern bool fMasternodeMode;
extern bool fDisableGovernance;
extern int nWalletBackups;

// Application startup time (used for uptime calculation)
int64_t GetStartupTime();

/** Signals for translation. */
class CTranslationInterface
{
public:
    /** Translate a message to the native language of the user. */
    boost::signals2::signal<std::string (const char* psz)> Translate;
};

extern CTranslationInterface translationInterface;

extern const char * const BITCOIN_CONF_FILENAME;
extern const char * const BITCOIN_PID_FILENAME;

/**
 * Translation function: Call Translate signal on UI interface, which returns a boost::optional result.
 * If no translation slot is registered, nothing is returned, and simply return the input.
 */
inline std::string _(const char* psz)
{
    boost::optional<std::string> rv = translationInterface.Translate(psz);
    return rv ? (*rv) : psz;
}

void SetupEnvironment();
bool SetupNetworking();

template<typename... Args>
bool error(const char* fmt, const Args&... args)
{
    LogPrintStr("ERROR: " + SafeStringFormat(fmt, args...) + "\n");
    return false;
}

void PrintExceptionContinue(const std::exception_ptr pex, const char* pszExceptionOrigin);
bool FileCommit(FILE *file);
bool TruncateFile(FILE *file, unsigned int length);
int RaiseFileDescriptorLimit(int nMinFD);
void AllocateFileRange(FILE *file, unsigned int offset, unsigned int length);
bool RenameOver(fs::path src, fs::path dest);
bool LockDirectory(const fs::path& directory, const std::string lockfile_name, bool probe_only=false);
bool DirIsWritable(const fs::path& directory);

/** Release all directory locks. This is used for unit testing only, at runtime
 * the global destructor will take care of the locks.
 */
void ReleaseDirectoryLocks();

bool TryCreateDirectories(const fs::path& p);
fs::path GetDefaultDataDir();
const fs::path &GetBlocksDir(bool fNetSpecific = true);
const fs::path &GetDataDir(bool fNetSpecific = true);
fs::path GetBackupsDir();
void ClearDatadirCache();
fs::path GetConfigFile(const std::string& confPath);
#ifndef WIN32
fs::path GetPidFile();
void CreatePidFile(const fs::path &path, pid_t pid);
#endif
#ifdef WIN32
fs::path GetSpecialFolderPath(int nFolder, bool fCreate = true);
#endif
void runCommand(const std::string& strCommand);

/**
 * Most paths passed as configuration arguments are treated as relative to
 * the datadir if they are not absolute.
 *
 * @param path The path to be conditionally prefixed with datadir.
 * @param net_specific Forwarded to GetDataDir().
 * @return The normalized path.
 */
fs::path AbsPathForConfigVal(const fs::path& path, bool net_specific = true);

inline bool IsSwitchChar(char c)
{
#ifdef WIN32
    return c == '-' || c == '/';
#else
    return c == '-';
#endif
}

enum class OptionsCategory
{
    OPTIONS,
    CONNECTION,
    INDEXING,
    MASTERNODE,
    INSTANTSEND,
    STATSD,
    WALLET,
    WALLET_FEE,
    WALLET_HD,
    WALLET_KEEPASS,
    WALLET_COINJOIN,
    WALLET_DEBUG_TEST,
    ZMQ,
    DEBUG_TEST,
    CHAINPARAMS,
    NODE_RELAY,
    BLOCK_CREATION,
    RPC,
    GUI,
    COMMANDS,
    REGISTER_COMMANDS,
    POS,
    POW
};

class ArgsManager
{
protected:
    friend class ArgsManagerHelper;

    mutable CCriticalSection cs_args;
    std::map<std::string, std::vector<std::string>> m_override_args;
    std::map<std::string, std::vector<std::string>> m_config_args;
    std::string m_network;
    std::set<std::string> m_network_only_args;
    std::map<std::pair<OptionsCategory, std::string>, std::pair<std::string, bool>> m_available_args;

    void ReadConfigStream(std::istream& stream);

public:
    ArgsManager();

    /**
     * Select the network in use
     */
    void SelectConfigNetwork(const std::string& network);

    void ParseParameters(int argc, const char*const argv[]);
    void ReadConfigFile(const std::string& confPath);

    /**
     * Log warnings for options in m_section_only_args when
     * they are specified in the default section but not overridden
     * on the command line or in a network-specific section in the
     * config file.
     */
    void WarnForSectionOnlyArgs();

    /**
     * Return a vector of strings of the given argument
     *
     * @param strArg Argument to get (e.g. "-foo")
     * @return command-line arguments
     */
    std::vector<std::string> GetArgs(const std::string& strArg) const;

    /**
     * Return true if the given argument has been manually set
     *
     * @param strArg Argument to get (e.g. "-foo")
     * @return true if the argument has been set
     */
    bool IsArgSet(const std::string& strArg) const;

    /**
     * Return true if the argument was originally passed as a negated option,
     * i.e. -nofoo.
     *
     * @param strArg Argument to get (e.g. "-foo")
     * @return true if the argument was passed negated
     */
    bool IsArgNegated(const std::string& strArg) const;

    /**
     * Return string argument or default value
     *
     * @param strArg Argument to get (e.g. "-foo")
     * @param strDefault (e.g. "1")
     * @return command-line argument or default value
     */
    std::string GetArg(const std::string& strArg, const std::string& strDefault) const;

    /**
     * Return integer argument or default value
     *
     * @param strArg Argument to get (e.g. "-foo")
     * @param nDefault (e.g. 1)
     * @return command-line argument (0 if invalid number) or default value
     */
    int64_t GetArg(const std::string& strArg, int64_t nDefault) const;

    /**
     * Return boolean argument or default value
     *
     * @param strArg Argument to get (e.g. "-foo")
     * @param fDefault (true or false)
     * @return command-line argument or default value
     */
    bool GetBoolArg(const std::string& strArg, bool fDefault) const;

    /**
     * Set an argument if it doesn't already have a value
     *
     * @param strArg Argument to set (e.g. "-foo")
     * @param strValue Value (e.g. "1")
     * @return true if argument gets set, false if it already had a value
     */
    bool SoftSetArg(const std::string& strArg, const std::string& strValue);

    /**
     * Set a boolean argument if it doesn't already have a value
     *
     * @param strArg Argument to set (e.g. "-foo")
     * @param fValue Value (e.g. false)
     * @return true if argument gets set, false if it already had a value
     */
    bool SoftSetBoolArg(const std::string& strArg, bool fValue);

    // Forces an arg setting. Called by SoftSetArg() if the arg hasn't already
    // been set. Also called directly in testing.
    void ForceSetArg(const std::string& strArg, const std::string& strValue);
    void ForceRemoveArg(const std::string& strArg);

    /**
     * Looks for -regtest, -testnet and returns the appropriate BIP70 chain name.
     * @return CBaseChainParams::MAIN by default; raises runtime error if an invalid combination is given.
     */
    std::string GetChainName() const;

    /**
     * Looks for -devnet and returns either "devnet-<name>" or simply "devnet" if no name was specified.
     * This function should never be called for non-devnets.
     * @return either "devnet-<name>" or "devnet"; raises runtime error if no -devent was specified.
     */
    std::string GetDevNetName() const;

    /**
     * Add argument
     */
    void AddArg(const std::string& name, const std::string& help, const bool debug_only, const OptionsCategory& cat);

    /**
     * Get the help string
     */
    std::string GetHelpMessage();
};

extern ArgsManager gArgs;

/**
 * Format a string to be used as group of options in help messages
 *
 * @param message Group name (e.g. "RPC server options:")
 * @return the formatted string
 */
std::string HelpMessageGroup(const std::string& message);

/**
 * Format a string to be used as option description in help messages
 *
 * @param option Option message (e.g. "-rpcuser=<user>")
 * @param message Option description (e.g. "Username for JSON-RPC connections")
 * @return the formatted string
 */
std::string HelpMessageOpt(const std::string& option, const std::string& message);

/**
 * Return the number of cores available on the current system.
 * @note This does count virtual cores, such as those provided by HyperThreading.
 */
int GetNumCores();

void RenameThread(const char* name);
std::string GetThreadName();

namespace ctpl {
    class thread_pool;
}
void RenameThreadPool(ctpl::thread_pool& tp, const char* baseName);

/**
 * .. and a wrapper that just calls func once
 */
template <typename Callable> void TraceThread(const std::string name,  Callable func)
{
    std::string s = "cosa-" + name;
    RenameThread(s.c_str());
    try
    {
        LogPrintf("%s thread start\n", name);
        func();
        LogPrintf("%s thread exit\n", name);
    }
    catch (const boost::thread_interrupted&)
    {
        LogPrintf("%s thread interrupt\n", name);
        throw;
    }
    catch (...) {
        PrintExceptionContinue(std::current_exception(), name.c_str());
        throw;
    }
}

std::string CopyrightHolders(const std::string& strPrefix, unsigned int nStartYear, unsigned int nEndYear);

/**
 * @brief Converts version strings to 4-byte unsigned integer
 * @param strVersion version in "x.x.x" format (decimal digits only)
 * @return 4-byte unsigned integer, most significant byte is always 0
 * Throws std::bad_cast if format doesn\t match.
 */
uint32_t StringVersionToInt(const std::string& strVersion);


/**
 * @brief Converts version as 4-byte unsigned integer to string
 * @param nVersion 4-byte unsigned integer, most significant byte is always 0
 * @return version string in "x.x.x" format (last 3 bytes as version parts)
 * Throws std::bad_cast if format doesn\t match.
 */
std::string IntVersionToString(uint32_t nVersion);


/**
 * @brief Copy of the IntVersionToString, that returns "Invalid version" string
 * instead of throwing std::bad_cast
 * @param nVersion 4-byte unsigned integer, most significant byte is always 0
 * @return version string in "x.x.x" format (last 3 bytes as version parts)
 * or "Invalid version" if can't cast the given value
 */
std::string SafeIntVersionToString(uint32_t nVersion);


#endif // BITCOIN_UTIL_H
