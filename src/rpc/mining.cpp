// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "amount.h"
#include "chainparams.h"
#include "consensus/consensus.h"
#include "consensus/validation.h"
#include "core_io.h"
#ifdef ENABLE_MINING
#include "crypto/equihash.h"
#endif
#include "init.h"
#include "main.h"
#include "metrics.h"
#include "miner.h"
#include "net.h"
#include "pow.h"
#include "rpc/server.h"
#include "txmempool.h"
#include "util.h"
#include "validationinterface.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

#include <stdint.h>

#include <boost/assign/list_of.hpp>

#include <univalue.h>

using namespace std;

#include "zen/forkmanager.h"
using namespace zen;

/**
 * Return average network hashes per second based on the last 'lookup' blocks,
 * or over the difficulty averaging window if 'lookup' is nonpositive.
 * If 'height' is nonnegative, compute the estimate at the time when a given block was found.
 */
int64_t GetNetworkHashPS(int lookup, int height) {
    CBlockIndex *pb = chainActive.Tip();

    if (height >= 0 && height < chainActive.Height())
        pb = chainActive[height];

    if (pb == NULL || !pb->nHeight)
        return 0;

    // If lookup is nonpositive, then use difficulty averaging window.
    if (lookup <= 0)
        lookup = Params().GetConsensus().nPowAveragingWindow;

    // If lookup is larger than chain, then set it to chain length.
    if (lookup > pb->nHeight)
        lookup = pb->nHeight;

    CBlockIndex *pb0 = pb;
    int64_t minTime = pb0->GetBlockTime();
    int64_t maxTime = minTime;
    for (int i = 0; i < lookup; i++) {
        pb0 = pb0->pprev;
        int64_t time = pb0->GetBlockTime();
        minTime = std::min(time, minTime);
        maxTime = std::max(time, maxTime);
    }

    // In case there's a situation where minTime == maxTime, we don't want a divide by zero exception.
    if (minTime == maxTime)
        return 0;

    arith_uint256 workDiff = pb->nChainWork - pb0->nChainWork;
    int64_t timeDiff = maxTime - minTime;

    return (int64_t)(workDiff.getdouble() / timeDiff);
}

UniValue getlocalsolps(const UniValue& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error(
            "getlocalsolps\n"
            "\nReturns the average local solutions per second since this node was started.\n"
            "This is the same information shown on the metrics screen (if enabled).\n"
            
            "\nResult:\n"
            "xxxx     (numeric) solutions per second average\n"
            
            "\nExamples:\n"
            + HelpExampleCli("getlocalsolps", "")
            + HelpExampleRpc("getlocalsolps", "")
       );

    LOCK(cs_main);
    return GetLocalSolPS();
}

UniValue getnetworksolps(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "getnetworksolps ( blocks height )\n"
            "\nReturns the estimated network solutions per second based on the last n blocks.\n"
            "Pass in [blocks] to override # of blocks, -1 specifies over difficulty averaging window.\n"
            "Pass in [height] to estimate the network speed at the time when a certain block was found.\n"
            
            "\nArguments:\n"
            "1. blocks     (numeric, optional, default=120) the number of blocks, or -1 for blocks over difficulty averaging window\n"
            "2. height     (numeric, optional, default=-1) to estimate at the time of the given height\n"
            
            "\nResult:\n"
            "x             (numeric) solutions per second estimated\n"
            
            "\nExamples:\n"
            + HelpExampleCli("getnetworksolps", "")
            + HelpExampleRpc("getnetworksolps", "")
       );

    LOCK(cs_main);
    return GetNetworkHashPS(params.size() > 0 ? params[0].get_int() : 120, params.size() > 1 ? params[1].get_int() : -1);
}

UniValue getnetworkhashps(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "getnetworkhashps ( blocks height )\n"
            "\nDEPRECATED - left for backwards-compatibility. Use getnetworksolps instead.\n"
            "\nReturns the estimated network solutions per second based on the last n blocks.\n"
            "Pass in [blocks] to override # of blocks, -1 specifies over difficulty averaging window.\n"
            "Pass in [height] to estimate the network speed at the time when a certain block was found.\n"
            
            "\nArguments:\n"
            "1. blocks     (numeric, optional, default=120) the number of blocks, or -1 for blocks over difficulty averaging window\n"
            "2. height     (numeric, optional, default=-1) to estimate at the time of the given height\n"
            
            "\nResult:\n"
            "x             (numeric) solutions per second estimated\n"
            
            "\nExamples:\n"
            + HelpExampleCli("getnetworkhashps", "")
            + HelpExampleRpc("getnetworkhashps", "")
       );

    LOCK(cs_main);
    return GetNetworkHashPS(params.size() > 0 ? params[0].get_int() : 120, params.size() > 1 ? params[1].get_int() : -1);
}

#ifdef ENABLE_MINING
UniValue getgenerate(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getgenerate\n"
            "\nReturn if the server is set to generate coins or not. The default is false.\n"
            "It is set with the command line argument -gen (or zen.conf setting gen)\n"
            "It can also be set with the setgenerate call.\n"
            
            "\nResult\n"
            "true|false      (boolean) if the server is set to generate coins or not\n"
            
            "\nExamples:\n"
            + HelpExampleCli("getgenerate", "")
            + HelpExampleRpc("getgenerate", "")
        );
    LOCK(cs_main);
    return GetBoolArg("-gen", false);
}

UniValue generate(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 1)
        throw runtime_error(
            "generate numblocks\n"
            "\nMine blocks immediately (before the RPC call returns).\n"
            "\nNote: this function can only be used on the regtest network.\n"
            
            "\nArguments:\n"
            "1. numblocks        (numeric) how many blocks are generated immediately\n"
            
            "\nResult\n"
            "[                   (array) hashes of blocks generated\n"
                "\"hash\":       (string) hash of the block\n"
                ",...\n"
            "]\n"     
            
            "\nExamples:\n"
            "\nGenerate 11 blocks\n"
            + HelpExampleCli("generate", "11")
            + HelpExampleRpc("generate", "11")
        );

    if (GetArg("-mineraddress", "").empty()) {
#ifdef ENABLE_WALLET
        if (!pwalletMain) {
            throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Wallet disabled and -mineraddress not set");
        }
#else
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "zend compiled without wallet and -mineraddress not set");
#endif
    }
    if (!Params().MineBlocksOnDemand())
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "This method can only be used on regtest");

    int nHeightStart = 0;
    int nHeightEnd = 0;
    int nHeight = 0;
    int nGenerate = params[0].get_int();
#ifdef ENABLE_WALLET
    CReserveKey reservekey(pwalletMain);
#endif

    {   // Don't keep cs_main locked
        LOCK(cs_main);
        nHeightStart = chainActive.Height();
        nHeight = nHeightStart;
        nHeightEnd = nHeightStart+nGenerate;
    }
    unsigned int nExtraNonce = 0;
    UniValue blockHashes(UniValue::VARR);
    while (nHeight < nHeightEnd)
    {
#ifdef ENABLE_WALLET
        std::unique_ptr<CBlockTemplate> pblocktemplate(CreateNewBlockWithKey(reservekey));
#else
        std::unique_ptr<CBlockTemplate> pblocktemplate(CreateNewBlockWithKey());
#endif
        if (!pblocktemplate.get())
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Wallet keypool empty");
        CBlock *pblock = &pblocktemplate->block;
        {
            LOCK(cs_main);
            IncrementExtraNonce(pblock, chainActive.Tip(), nExtraNonce);
        }

        generateEquihash(*pblock);

        CValidationState state;
        if (!ProcessNewBlock(state, NULL, pblock, true, NULL))
            throw JSONRPCError(RPC_INTERNAL_ERROR, "ProcessNewBlock, block not accepted");
        ++nHeight;
        blockHashes.push_back(pblock->GetHash().GetHex());
    }
    return blockHashes;
}


UniValue setgenerate(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "setgenerate generate ( genproclimit )\n"
            "\nSet 'generate' true or false to turn generation on or off.\n"
            "Generation is limited to 'genproclimit' processors, -1 is unlimited.\n"
            "See the getgenerate call for the current setting.\n"
            
            "\nArguments:\n"
            "1. generate         (boolean, required) set to true to turn on generation, off to turn off\n"
            "2. genproclimit     (numeric, optional) set the processor limit for when generation is on. Can be -1 for unlimited\n"
            "\nResult:\n"
            "Nothing\n"
            
            "\nExamples:\n"
            "\nSet the generation on with a limit of one processor\n"
            + HelpExampleCli("setgenerate", "true 1") +
            "\nCheck the setting\n"
            + HelpExampleCli("getgenerate", "") +
            "\nTurn off generation\n"
            + HelpExampleCli("setgenerate", "false") +
            "\nUsing json rpc\n"
            + HelpExampleRpc("setgenerate", "true, 1")
        );

    if (GetArg("-mineraddress", "").empty()) {
#ifdef ENABLE_WALLET
        if (!pwalletMain) {
            throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Wallet disabled and -mineraddress not set");
        }
#else
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "zend compiled without wallet and -mineraddress not set");
#endif
    }
    if (Params().MineBlocksOnDemand())
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Use the generate method instead of setgenerate on this network");

    bool fGenerate = true;
    if (params.size() > 0)
        fGenerate = params[0].get_bool();

    int nGenProcLimit = -1;
    if (params.size() > 1)
    {
        nGenProcLimit = params[1].get_int();
        if (nGenProcLimit == 0)
            fGenerate = false;
    }

    mapArgs["-gen"] = (fGenerate ? "1" : "0");
    mapArgs ["-genproclimit"] = itostr(nGenProcLimit);
#ifdef ENABLE_WALLET
    GenerateBitcoins(fGenerate, pwalletMain, nGenProcLimit);
#else
    GenerateBitcoins(fGenerate, nGenProcLimit);
#endif

    return NullUniValue;
}
#endif


UniValue getmininginfo(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getmininginfo\n"
            "\nReturns a json object containing mining-related information."
            
            "\nResult:\n"
            "{\n"
            "  \"blocks\": nnn,                  (numeric) the current block\n"
            "  \"currentblocksize\": nnn,        (numeric) the last block size\n"
            "  \"currentblocktx\": nnn,          (numeric) number of transactions in the last block\n"
            "  \"currentblockcert\": nnn,        (numeric) number of certificates in the last block\n"
            "  \"difficulty\": xxxxxxxx,         (numeric) the current difficulty\n"
            "  \"errors\": \"...\",              (string) current errors\n"
            "  \"generate\": true|false,         (boolean) if the generation is on or off (see getgenerate or setgenerate calls)\n"
            "  \"genproclimit\": n,              (numeric) the processor limit for generation. -1 if no generation. (see getgenerate or setgenerate calls)\n"
            "  \"localsolps\": xxxxxxxx,         (numeric) the average local solution rate in Sol/s since this node was started\n"
            "  \"networksolps\": x,              (numeric) the estimated network solution rate in Sol/s\n"
            "  \"pooledtx\": n,                  (numeric) the number of txes in the mem pool\n"
            "  \"pooledcert\": n,                (numeric) the number of certs in the mem pool\n"
            "  \"testnet\": true|false,          (boolean) if using testnet or not\n"
            "  \"chain\": \"xxxx\"               (string) current network name as defined in BIP70 (main, test, regtest)\n"
            "}\n"
            
            "\nExamples:\n"
            + HelpExampleCli("getmininginfo", "")
            + HelpExampleRpc("getmininginfo", "")
        );


    LOCK(cs_main);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("blocks",           (int)chainActive.Height());
    obj.pushKV("currentblocksize", (uint64_t)nLastBlockSize);
    obj.pushKV("currentblocktx",   (uint64_t)nLastBlockTx);
    obj.pushKV("currentblockcert", (uint64_t)nLastBlockCert);
    obj.pushKV("difficulty",       (double)GetNetworkDifficulty());
    obj.pushKV("errors",           GetWarnings("statusbar"));
    obj.pushKV("genproclimit",     (int)GetArg("-genproclimit", -1));
    obj.pushKV("localsolps"  ,     getlocalsolps(params, false));
    obj.pushKV("networksolps",     getnetworksolps(params, false));
    obj.pushKV("networkhashps",    getnetworksolps(params, false));
    obj.pushKV("pooledtx",         (uint64_t)mempool.sizeTx());
    obj.pushKV("pooledcert",       (uint64_t)mempool.sizeCert());
    obj.pushKV("testnet",          Params().TestnetToBeDeprecatedFieldRPC());
    obj.pushKV("chain",            Params().NetworkIDString());
#ifdef ENABLE_MINING
    obj.pushKV("generate",         getgenerate(params, false));
#endif
    return obj;
}


// NOTE: Unlike wallet RPC (which use BTC values), mining RPCs follow GBT (BIP 22) in using satoshi amounts
UniValue prioritisetransaction(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 3)
        throw runtime_error(
            "prioritisetransaction <txid> <priority delta> <fee delta>\n"
            "Accepts the transaction into mined blocks at a higher (or lower) priority\n"
            
            "\nArguments:\n"
            "1. \"txid\"       (string, required) the transaction id\n"
            "2. priority delta (numeric, required) the priority to add or subtract\n"
            "                   the transaction selection algorithm considers the tx as it would have a higher priority\n"
            "                  (priority of a transaction is calculated: coinage * value_in_satoshis / txsize) \n"
            "3. fee delta      (numeric, required) the fee value (in satoshis) to add (or subtract, if negative)\n"
            "                   the fee is not actually paid, only the algorithm for selecting transactions into a block\n"
            "                   considers the transaction as it would have paid a higher (or lower) fee\n"
            
            "\nResult\n"
            "true              (boolean) returns true\n"
            
            "\nExamples:\n"
            + HelpExampleCli("prioritisetransaction", "\"txid\" 0.0 10000")
            + HelpExampleRpc("prioritisetransaction", "\"txid\", 0.0, 10000")
        );

    LOCK(cs_main);

    uint256 hash = ParseHashStr(params[0].get_str(), "txid");
    CAmount nAmount = params[2].get_int64();

    mempool.PrioritiseTransaction(hash, params[0].get_str(), params[1].get_real(), nAmount);
    return true;
}


// NOTE: Assumes a conclusive result; if result is inconclusive, it must be handled by caller
static UniValue BIP22ValidationResult(const CValidationState& state)
{
    if (state.IsValid())
        return NullUniValue;

    std::string strRejectReason = state.GetRejectReason();
    if (state.IsError())
        throw JSONRPCError(RPC_VERIFY_ERROR, strRejectReason);
    if (state.IsInvalid())
    {
        if (strRejectReason.empty())
            return "rejected";
        return strRejectReason;
    }
    // Should be impossible
    return "valid?";
}

UniValue getblocktemplate(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "getblocktemplate ( \"jsonrequestobject\" )\n"
            "\nIf the request parameters include a 'mode' key, that is used to explicitly select between the default 'template' request or a 'proposal'.\n"
            "It returns data needed to construct a block to work on.\n"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.\n"

            "\nArguments:\n"
            "1. \"jsonrequestobject\"                   (string, optional) a json object in the following spec\n"
            "     {\n"
            "       \"mode\":\"template\"               (string, optional) this must be set to \"template\" or omitted\n"
            "       \"capabilities\":[                  (array, optional) a list of strings\n"
            "           \"support\"                     (string) client side supported feature, 'longpoll', 'coinbasetxn', 'coinbasevalue', 'proposal', 'serverlist', 'workid'\n"
            "           ,...\n"
            "         ]\n"
            "     }\n"
            "\n"

            "\nResult:\n"
            "{\n"
            "  \"version\" : n,                         (numeric) the block version\n"
            "  \"previousblockhash\" : \"xxxx\",        (string) the hash of current highest block\n"
            "  \"transactions\" : [                     (array) contents of non-coinbase transactions that should be included in the next block\n"
            "      {\n"
            "         \"data\" : \"xxxx\",              (string) transaction data encoded in hexadecimal (byte-for-byte)\n"
            "         \"hash\" : \"xxxx\",              (string) hash/id encoded in little-endian hexadecimal\n"
            "         \"depends\" : [                   (array) array of numbers \n"
            "             n                             (numeric) transactions before this one (by 1-based index in 'transactions' list) that must be present in the final block if this one is\n"
            "             ,...\n"
            "         ],\n"
            "         \"fee\": n,                       (numeric) difference in value between transaction inputs and outputs (in Satoshis); for coinbase transactions, this is a negative Number of the total collected block fees (ie, not including the block subsidy); if key is not present, fee is unknown and clients MUST NOT assume there isn't one\n"
            "         \"sigops\" : n,                   (numeric) total number of SigOps, as counted for purposes of block limits; if key is not present, sigop count is unknown and clients MUST NOT assume there aren't any\n"
            "         \"required\" : true|false         (boolean) if provided and true, this transaction must be in the final block\n"
            "      }\n"
            "      ,...\n"
            "  ],\n"
//            "  \"coinbaseaux\" : {                    (json object) data that should be included in the coinbase's scriptSig content\n"
//            "      \"flags\" : \"flags\"              (string) \n"
//            "  },\n"
//            "  \"coinbasevalue\" : n,                 (numeric) maximum allowable input to coinbase transaction, including the generation award and transaction fees (in Satoshis)\n"
            "  \"coinbasetxn\" : { ... },               (json object) information for coinbase transaction\n"
            "  \"longpollid\": \"xxxx\"                 (string) id to wait for"
            "  \"target\" : \"xxxx\",                   (string) the hash target\n"
            "  \"mintime\" : xxx,                       (numeric) the minimum timestamp appropriate for next block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"mutable\" : [                          (array of string) list of ways the block template may be changed \n"
            "     \"value\"                             (string) a way the block template may be changed, e.g. 'time', 'transactions', 'prevblock'\n"
            "     ,...\n"
            "  ],\n"
            "  \"noncerange\" : \"00000000ffffffff\",   (string) a range of valid nonces\n"
            "  \"sigoplimit\" : n,                      (numeric) limit of sigops in blocks\n"
            "  \"sizelimit\" : n,                       (numeric) limit of block size\n"
            "  \"curtime\" : ttt,                       (numeric) current timestamp in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"bits\" : \"xxx\",                      (string) compressed target of next block\n"
            "  \"height\" : n                           (numeric) the height of the next block\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("getblocktemplate", "")
            + HelpExampleRpc("getblocktemplate", "")
         );

    LOCK(cs_main);

    // Wallet or miner address is required because we support coinbasetxn
    if (GetArg("-mineraddress", "").empty()) {
#ifdef ENABLE_WALLET
        if (!pwalletMain) {
            throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Wallet disabled and -mineraddress not set");
        }
#else
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "zend compiled without wallet and -mineraddress not set");
#endif
    }

    int nHeight = chainActive.Height() + 1;
    bool certSupported = ForkManager::getInstance().areSidechainsSupported(nHeight);

    std::string strMode = "template";
    UniValue lpval = NullUniValue;
    // TODO: Re-enable coinbasevalue once a specification has been written
    bool coinbasetxn = true;
    if (params.size() > 0)
    {
        const UniValue& oparam = params[0].get_obj();
        const UniValue& modeval = find_value(oparam, "mode");
        if (modeval.isStr())
            strMode = modeval.get_str();
        else if (modeval.isNull())
        {
            /* Do nothing */
        }
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");
        lpval = find_value(oparam, "longpollid");

        if (strMode == "proposal")
        {
            const UniValue& dataval = find_value(oparam, "data");
            if (!dataval.isStr())
                throw JSONRPCError(RPC_TYPE_ERROR, "Missing data String key for proposal");

            CBlock block;
            if (!DecodeHexBlk(block, dataval.get_str()))
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");

            uint256 hash = block.GetHash();
            BlockMap::iterator mi = mapBlockIndex.find(hash);
            if (mi != mapBlockIndex.end()) {
                CBlockIndex *pindex = mi->second;
                if (pindex->IsValid(BLOCK_VALID_SCRIPTS))
                    return "duplicate";
                if (pindex->nStatus & BLOCK_FAILED_MASK)
                    return "duplicate-invalid";
                return "duplicate-inconclusive";
            }

            CBlockIndex* const pindexPrev = chainActive.Tip();
            // TestBlockValidity only supports blocks built on the current Tip
            if (block.hashPrevBlock != pindexPrev->GetBlockHash())
                return "inconclusive-not-best-prevblk";
            CValidationState state;
            TestBlockValidity(state, block, pindexPrev, flagCheckPow::OFF, flagCheckMerkleRoot::ON, flagScRelatedChecks::ON);
            return BIP22ValidationResult(state);
        }
    }

    if (strMode != "template")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");

    /* for testing, comment this block out if using just one node */
    if (vNodes.empty())
        throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Horizen is not connected!");

    if (IsInitialBlockDownload())
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Horizen is downloading blocks...");

    static unsigned int nTransactionsUpdatedLast;

    if (!lpval.isNull())
    {
        // Wait to respond until either the best block changes, OR a minute has passed and there are more transactions
        uint256 hashWatchedChain;
        boost::system_time checktxtime;
        unsigned int nTransactionsUpdatedLastLP;

        if (lpval.isStr())
        {
            // Format: <hashBestChain><nTransactionsUpdatedLast>
            std::string lpstr = lpval.get_str();

            hashWatchedChain.SetHex(lpstr.substr(0, 64));
            nTransactionsUpdatedLastLP = atoi64(lpstr.substr(64));
        }
        else
        {
            // NOTE: Spec does not specify behaviour for non-string longpollid, but this makes testing easier
            hashWatchedChain = chainActive.Tip()->GetBlockHash();
            nTransactionsUpdatedLastLP = nTransactionsUpdatedLast;
        }

        // Release the wallet and main lock while waiting
        LEAVE_CRITICAL_SECTION(cs_main);
        {
            checktxtime = boost::get_system_time() + boost::posix_time::minutes(1);

            boost::unique_lock<boost::mutex> lock(csBestBlock);
            while (chainActive.Tip()->GetBlockHash() == hashWatchedChain && IsRPCRunning())
            {
                if (!cvBlockChange.timed_wait(lock, checktxtime))
                {
                    // Timeout: Check transactions for update
                    if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLastLP)
                        break;
                    checktxtime += boost::posix_time::seconds(10);
                }
            }
        }
        ENTER_CRITICAL_SECTION(cs_main);

        if (!IsRPCRunning())
            throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
        // TODO: Maybe recheck connections/IBD and (if something wrong) send an expires-immediately template to stop miners?
    }

    // Update block
    static CBlockIndex* pindexPrev;
    static int64_t nStart;
    static CBlockTemplate* pblocktemplate;
    if (pindexPrev != chainActive.Tip() ||
        (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 5))
    {
        // Clear pindexPrev so future calls make a new block, despite any failures from here on
        pindexPrev = NULL;

        // Store the pindexBest used before CreateNewBlockWithKey, to avoid races
        nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
        CBlockIndex* pindexPrevNew = chainActive.Tip();
        nStart = GetTime();

        // Create new block
        if(pblocktemplate)
        {
            delete pblocktemplate;
            pblocktemplate = NULL;
        }
#ifdef ENABLE_WALLET
        CReserveKey reservekey(pwalletMain);
        pblocktemplate = CreateNewBlockWithKey(reservekey);
#else
        pblocktemplate = CreateNewBlockWithKey();
#endif
        if (!pblocktemplate)
            throw JSONRPCError(RPC_OUT_OF_MEMORY, "Out of memory");

        // Need to update only after we know CreateNewBlockWithKey succeeded
        pindexPrev = pindexPrevNew;
    }
    CBlock* pblock = &pblocktemplate->block; // pointer for convenience

    // Update nTime
    UpdateTime(pblock, Params().GetConsensus(), pindexPrev);
    pblock->nNonce = uint256();

    UniValue aCaps(UniValue::VARR); aCaps.push_back("proposal");

    UniValue txCoinbase = NullUniValue;
    UniValue transactions(UniValue::VARR);
    map<uint256, int64_t> setTxIndex;
    int i = 0;
    BOOST_FOREACH (const CTransaction& tx, pblock->vtx) {
        uint256 txHash = tx.GetHash();
        setTxIndex[txHash] = i++;

        if (tx.IsCoinBase() && !coinbasetxn)
            continue;

        UniValue entry(UniValue::VOBJ);

        entry.pushKV("data", EncodeHexTx(tx));

        entry.pushKV("hash", txHash.GetHex());

        UniValue deps(UniValue::VARR);
        BOOST_FOREACH (const CTxIn &in, tx.GetVin())
        {
            if (setTxIndex.count(in.prevout.hash))
                deps.push_back(setTxIndex[in.prevout.hash]);
        }
        entry.pushKV("depends", deps);

        int index_in_template = i - 1;
        entry.pushKV("fee", pblocktemplate->vTxFees[index_in_template]);
        entry.pushKV("sigops", pblocktemplate->vTxSigOps[index_in_template]);

        if (tx.IsCoinBase()) {
            // Show community reward if it is required
            if (pblock->vtx[0].GetVout().size() > 1) {
                // Correct this if GetBlockTemplate changes the order
                entry.pushKV("communityfund", (int64_t)tx.GetVout()[1].nValue);
                if (pblock->vtx[0].GetVout().size() > 3) {
                    entry.pushKV("securenodes", (int64_t)tx.GetVout()[2].nValue);
                    entry.pushKV("supernodes", (int64_t)tx.GetVout()[3].nValue);
                }
            }
            entry.pushKV("required", true);
            txCoinbase = entry;
        } else {
            transactions.push_back(entry);
        }
    }

    UniValue aux(UniValue::VOBJ);
    aux.pushKV("flags", HexStr(COINBASE_FLAGS.begin(), COINBASE_FLAGS.end()));

    arith_uint256 hashTarget = arith_uint256().SetCompact(pblock->nBits);

    static UniValue aMutable(UniValue::VARR);
    if (aMutable.empty())
    {
        aMutable.push_back("time");
        aMutable.push_back("transactions");
        if (certSupported)
        {
            aMutable.push_back("certificates");
        }
        aMutable.push_back("prevblock");
    }

    UniValue result(UniValue::VOBJ);

    // return merkleTree and scTxsCommitment
    CCoinsViewCache view(pcoinsTip);
    uint256 merkleTree = pblock->BuildMerkleTree();
    uint256 scTxsCommitment;
    scTxsCommitment.SetNull();
    if (certSupported) {
        scTxsCommitment = pblock->BuildScTxsCommitment(view);
    }

    result.pushKV("merkleTree", merkleTree.ToString());
    result.pushKV("scTxsCommitment", scTxsCommitment.ToString());
    result.pushKV("capabilities", aCaps);
    result.pushKV("version", pblock->nVersion);
    result.pushKV("previousblockhash", pblock->hashPrevBlock.GetHex());
    result.pushKV("transactions", transactions);
    if (certSupported)
    {
        UniValue certificates(UniValue::VARR);
        int cert_idx_in_template = 0;
        BOOST_FOREACH (const CScCertificate& cert, pblock->vcert) {
            uint256 certHash = cert.GetHash();
            UniValue entry(UniValue::VOBJ);
 
            entry.pushKV("data", EncodeHexCert(cert));
            entry.pushKV("hash", certHash.GetHex());
            // no depends for cert since there are no inputs
            entry.pushKV("fee", pblocktemplate->vCertFees[cert_idx_in_template]);
            entry.pushKV("sigops", pblocktemplate->vCertSigOps[cert_idx_in_template]);
            certificates.push_back(entry);
 
            cert_idx_in_template++;
        }
        result.pushKV("certificates", certificates);
    }

    if (coinbasetxn) {
        assert(txCoinbase.isObject());
        result.pushKV("coinbasetxn", txCoinbase);
    } else {
        result.pushKV("coinbaseaux", aux);
        result.pushKV("coinbasevalue", (int64_t)pblock->vtx[0].GetVout()[0].nValue);
    }

    unsigned int block_size_limit = MAX_BLOCK_SIZE;
    if (pblock->nVersion != BLOCK_VERSION_SC_SUPPORT)
        block_size_limit = MAX_BLOCK_SIZE_BEFORE_SC;

    result.pushKV("longpollid", chainActive.Tip()->GetBlockHash().GetHex() + i64tostr(nTransactionsUpdatedLast));
    result.pushKV("target", hashTarget.GetHex());
    result.pushKV("mintime", (int64_t)pindexPrev->GetMedianTimePast()+1);
    result.pushKV("mutable", aMutable);
    result.pushKV("noncerange", "00000000ffffffff");
    result.pushKV("sigoplimit", (int64_t)MAX_BLOCK_SIGOPS);
    result.pushKV("sizelimit", (int64_t)block_size_limit);
    result.pushKV("curtime", pblock->GetBlockTime());
    result.pushKV("bits", strprintf("%08x", pblock->nBits));
    result.pushKV("height", (int64_t)(pindexPrev->nHeight+1));

    return result;
}

class submitblock_StateCatcher : public CValidationInterface
{
public:
    uint256 hash;
    bool found;
    CValidationState state;

    submitblock_StateCatcher(const uint256 &hashIn) : hash(hashIn), found(false), state() {};

protected:
    virtual void BlockChecked(const CBlock& block, const CValidationState& stateIn) {
        if (block.GetHash() != hash)
            return;
        found = true;
        state = stateIn;
    };
};

UniValue submitblock(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "submitblock \"hexdata\" ( \"jsonparametersobject\" )\n"
            "\nAttempts to submit new block to network.\n"
            "The 'jsonparametersobject' parameter is currently ignored.\n"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.\n"

            "\nArguments\n"
            "1. \"hexdata\"                  (string, required) the hex-encoded block data to submit\n"
            "2. \"jsonparametersobject\"     (string, optional) object of optional parameters\n"
            "    {\n"
            "      \"workid\" : \"id\"       (string, optional) if the server provided a workid, it MUST be included with submissions\n"
            "    }\n"
            
            "\nResult:\n"
            "Nothing if success\n"
            "\"duplicate\" - node already has valid copy of block\n"
            "\"duplicate-invalid\" - node already has block, but it is invalid\n"
            "\"duplicate-inconclusive\" - node already has block but has not validated it\n"
            "\"inconclusive\" - node has not validated the block, it may not be on the node's current best chain\n"
            "\"rejected\" - block was rejected as invalid\n"
            "For more information on submitblock parameters and results, see: https://github.com/bitcoin/bips/blob/master/bip-0022.mediawiki#block-submission\n"
            
            "\nExamples:\n"
            + HelpExampleCli("submitblock", "\"mydata\"")
            + HelpExampleRpc("submitblock", "\"mydata\"")
        );

    CBlock block;
    if (!DecodeHexBlk(block, params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");

    uint256 hash = block.GetHash();
    bool fBlockPresent = false;
    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(hash);
        if (mi != mapBlockIndex.end()) {
            CBlockIndex *pindex = mi->second;
            if (pindex->IsValid(BLOCK_VALID_SCRIPTS))
                return "duplicate";
            if (pindex->nStatus & BLOCK_FAILED_MASK)
                return "duplicate-invalid";
            // Otherwise, we might only have the header - process the block before returning
            fBlockPresent = true;
        }
    }

    CValidationState state;
    submitblock_StateCatcher sc(block.GetHash());
    RegisterValidationInterface(&sc);
    bool fAccepted = ProcessNewBlock(state, NULL, &block, true, NULL);
    UnregisterValidationInterface(&sc);
    if (fBlockPresent)
    {
        if (fAccepted && !sc.found)
            return "duplicate-inconclusive";
        return "duplicate";
    }
    if (fAccepted)
    {
        if (!sc.found)
            return "inconclusive";
        state = sc.state;
    }
    return BIP22ValidationResult(state);
}

UniValue estimatefee(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "estimatefee nblocks\n"
            "\nEstimates the approximate fee per kilobyte\n"
            "needed for a transaction to begin confirmation\n"
            "within nblocks blocks.\n"

            "\nArguments:\n"
            "1. nblocks     (numeric) number of blocks\n"

            "\nResult:\n"
            "n :            (numeric) estimated fee-per-kilobyte\n"
            "\n"
            "-1.0 is returned if not enough transactions and\n"
            "blocks have been observed to make an estimate.\n"

            "\nExample:\n"
            + HelpExampleCli("estimatefee", "6")
            + HelpExampleRpc("estimatefee", "6")
        );

    RPCTypeCheck(params, boost::assign::list_of(UniValue::VNUM));

    int nBlocks = params[0].get_int();
    if (nBlocks < 1)
        nBlocks = 1;

    CFeeRate feeRate = mempool.estimateFee(nBlocks);
    if (feeRate == CFeeRate(0))
        return -1.0;

    return ValueFromAmount(feeRate.GetFeePerK());
}

UniValue estimatepriority(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "estimatepriority nblocks\n"
            "\nEstimates the approximate priority\n"
            "a zero-fee transaction needs to begin confirmation\n"
            "within nblocks blocks.\n"

            "\nArguments:\n"
            "1. nblocks     (numeric) number of blocks\n"
            
            "\nResult:\n"
            "n :            (numeric) estimated priority\n"
            "\n"
            "-1.0 is returned if not enough transactions and\n"
            "blocks have been observed to make an estimate.\n"

            "\nExample:\n"
            + HelpExampleCli("estimatepriority", "6")
            + HelpExampleRpc("estimatepriority", "6")
            );

    RPCTypeCheck(params, boost::assign::list_of(UniValue::VNUM));

    int nBlocks = params[0].get_int();
    if (nBlocks < 1)
        nBlocks = 1;

    return mempool.estimatePriority(nBlocks);
}

UniValue getblocksubsidy(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "getblocksubsidy height\n"
            "\nReturns block subsidy reward, taking into account the mining slow start and the community fund, of block at index provided.\n"
            
            "\nArguments:\n"
            "1. height                    (numeric, optional) the block height\n"
            "                              if not provided, defaults to the current height of the chain\n"
            
            "\nResult:\n"
            "{\n"
            "  \"miner\": xxxx,           (numeric) the mining reward amount in " + CURRENCY_UNIT + "\n"
            "  \"community\": xxxx,       (numeric) the community fund amount in " + CURRENCY_UNIT + "\n"
            "  \"securenodes\": xxxx,     (numeric) the securenodes fund amount in " + CURRENCY_UNIT + "\n"
            "  \"supernodes\": xxxx       (numeric) the supernodes fund amount in " + CURRENCY_UNIT + "\n"
            "}\n"
            
            "\nExamples:\n"
            + HelpExampleCli("getblocksubsidy", "1000")
            + HelpExampleRpc("getblocksubsidy", "1000")
        );

    LOCK(cs_main);
    int nHeight = (params.size()==1) ? params[0].get_int() : chainActive.Height();
    if (nHeight < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Block height out of range");

    CAmount nReward = GetBlockSubsidy(nHeight, Params().GetConsensus());
    CAmount minerReward = nReward;

    CAmount nCommunityFund = ForkManager::getInstance().getCommunityFundReward(nHeight,nReward, Fork::CommunityFundType::FOUNDATION);
    minerReward -= nCommunityFund;

    CAmount secureNodeFund = ForkManager::getInstance().getCommunityFundReward(nHeight,nReward, Fork::CommunityFundType::SECURENODE);
    if (secureNodeFund > 0) {
        minerReward -= secureNodeFund;
    }
    CAmount superNodeFund = ForkManager::getInstance().getCommunityFundReward(nHeight,nReward, Fork::CommunityFundType::SUPERNODE);
    if (superNodeFund > 0) {
        minerReward -= superNodeFund;
    }


    UniValue result(UniValue::VOBJ);
    result.pushKV("miner", ValueFromAmount(minerReward));
    result.pushKV("community", ValueFromAmount(nCommunityFund));
    if (secureNodeFund > 0) {
        result.pushKV("securenodes", ValueFromAmount(secureNodeFund));
    }
    if (superNodeFund > 0) {
        result.pushKV("supernodes", ValueFromAmount(superNodeFund));
    }

    return result;
}

UniValue getblockmerkleroots(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() != 2)
        throw runtime_error(
                "getblockmerkleroots transactions certificates\n"
                "\nReturns Merkleroot and ScTxsCommitment for the next block.\n"
                "\nArguments:\n"
                "1. transactions         (array) Array of raw transactions (HEX format).\n"
                "2. certificates         (array) Array of raw certificates (HEX format).\n"
                "\nResult:\n"
                "{\n"
                "  \"merkleTree\" : \"xxx\"           (string) Merkleroot calculated on transactions and certificates.\n"
                "  \"scTxsCommitment\" : \"xxxx\"      (string) scTxsCommitment calculated on certificates.\n"
                "}\n"
                "\nExamples:\n"
                + HelpExampleCli("getblockmerkleroots", "'[\"0100000001000000...\", ...]', '[\"0100000001000000...\", ...]'")
                + HelpExampleRpc("getblockmerkleroots", "'[\"0100000001000000...\", ...]', '[\"0100000001000000...\", ...]'")
        );
    LOCK(cs_main);

    int nHeight = chainActive.Height() + 1;
    bool certSupported = ForkManager::getInstance().areSidechainsSupported(nHeight);

    UniValue txsStr =  params[0].get_array();
    std::vector<CTransaction> txs;

    for (const UniValue & tx : txsStr.getValues()) {
    	CTransaction transaction;
        if (!DecodeHexTx(transaction, tx.get_str()))
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
        txs.push_back(transaction);
    }

    UniValue certsStr =  params[1].get_array();
    std::vector<CScCertificate> certs;

    for (const UniValue & cert : certsStr.getValues()) {
    	CScCertificate certificate;
        if (!DecodeHexCert(certificate, cert.get_str()))
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Certificate decode failed");
        certs.push_back(certificate);
    }

    // Create new block
    std::unique_ptr<CBlockTemplate> pblocktemplate(new CBlockTemplate());
    CBlock *pblock = &pblocktemplate->block; // pointer for convenience

    pblock->vtx = txs;
    pblock->vcert = certs;
    CCoinsViewCache view(pcoinsTip);

    uint256 merkleTree = pblock->BuildMerkleTree();
    uint256 scTxsCommitment;
    scTxsCommitment.SetNull();
    if (certSupported) {
        scTxsCommitment = pblock->BuildScTxsCommitment(view);
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("merkleTree", merkleTree.ToString());
    result.pushKV("scTxsCommitment", scTxsCommitment.ToString());

    return result;
}
