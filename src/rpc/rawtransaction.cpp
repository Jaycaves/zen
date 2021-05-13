// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "base58.h"
#include "consensus/validation.h"
#include "core_io.h"
#include "init.h"
#include "keystore.h"
#include "main.h"
#include "merkleblock.h"
#include "net.h"
#include "primitives/transaction.h"
#include "rpc/server.h"
#include "script/script.h"
#include "script/script_error.h"
#include "script/sign.h"
#include "script/standard.h"
#include "uint256.h"
#include "tinyformat.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

#include <stdint.h>
#include <string>

#include <boost/assign/list_of.hpp>

#include <univalue.h>
#include "sc/sidechain.h"
#include "sc/sidechainrpc.h"

using namespace std;

void ScriptPubKeyToJSON(const CScript& scriptPubKey, UniValue& out, bool fIncludeHex)
{
    txnouttype type;
    vector<CTxDestination> addresses;
    int nRequired;

    out.push_back(Pair("asm", scriptPubKey.ToString()));
    if (fIncludeHex)
        out.push_back(Pair("hex", HexStr(scriptPubKey.begin(), scriptPubKey.end())));

    if (!ExtractDestinations(scriptPubKey, type, addresses, nRequired)) {
        out.push_back(Pair("type", GetTxnOutputType(type)));
        return;
    }

    out.push_back(Pair("reqSigs", nRequired));
    out.push_back(Pair("type", GetTxnOutputType(type)));

    UniValue a(UniValue::VARR);
    BOOST_FOREACH(const CTxDestination& addr, addresses)
        a.push_back(CBitcoinAddress(addr).ToString());
    out.push_back(Pair("addresses", a));
}


UniValue TxJoinSplitToJSON(const CTransaction& tx) {
    bool useGroth = tx.nVersion == GROTH_TX_VERSION;
    UniValue vjoinsplit(UniValue::VARR);
    for (unsigned int i = 0; i < tx.GetVjoinsplit().size(); i++) {
        const JSDescription& jsdescription = tx.GetVjoinsplit()[i];
        UniValue joinsplit(UniValue::VOBJ);

        joinsplit.push_back(Pair("vpub_old", ValueFromAmount(jsdescription.vpub_old)));
        joinsplit.push_back(Pair("vpub_new", ValueFromAmount(jsdescription.vpub_new)));

        joinsplit.push_back(Pair("anchor", jsdescription.anchor.GetHex()));

        {
            UniValue nullifiers(UniValue::VARR);
            BOOST_FOREACH(const uint256 nf, jsdescription.nullifiers) {
                nullifiers.push_back(nf.GetHex());
            }
            joinsplit.push_back(Pair("nullifiers", nullifiers));
        }

        {
            UniValue commitments(UniValue::VARR);
            BOOST_FOREACH(const uint256 commitment, jsdescription.commitments) {
                commitments.push_back(commitment.GetHex());
            }
            joinsplit.push_back(Pair("commitments", commitments));
        }

        joinsplit.push_back(Pair("onetimePubKey", jsdescription.ephemeralKey.GetHex()));
        joinsplit.push_back(Pair("randomSeed", jsdescription.randomSeed.GetHex()));

        {
            UniValue macs(UniValue::VARR);
            BOOST_FOREACH(const uint256 mac, jsdescription.macs) {
                macs.push_back(mac.GetHex());
            }
            joinsplit.push_back(Pair("macs", macs));
        }

        CDataStream ssProof(SER_NETWORK, PROTOCOL_VERSION);
        auto ps = SproutProofSerializer<CDataStream>(ssProof, useGroth, SER_NETWORK, PROTOCOL_VERSION);
        boost::apply_visitor(ps, jsdescription.proof);
        joinsplit.push_back(Pair("proof", HexStr(ssProof.begin(), ssProof.end())));

        {
            UniValue ciphertexts(UniValue::VARR);
            for (const ZCNoteEncryption::Ciphertext ct : jsdescription.ciphertexts) {
                ciphertexts.push_back(HexStr(ct.begin(), ct.end()));
            }
            joinsplit.push_back(Pair("ciphertexts", ciphertexts));
        }

        vjoinsplit.push_back(joinsplit);
    }
    return vjoinsplit;
}

void TxToJSON(const CTransaction& tx, const uint256 hashBlock, UniValue& entry)
{
    entry.push_back(Pair("txid", tx.GetHash().GetHex()));
    entry.push_back(Pair("version", tx.nVersion));
    entry.push_back(Pair("locktime", (int64_t)tx.GetLockTime()));
    UniValue vin(UniValue::VARR);
    BOOST_FOREACH(const CTxIn& txin, tx.GetVin()) {
        UniValue in(UniValue::VOBJ);
        if (tx.IsCoinBase())
            in.push_back(Pair("coinbase", HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
        else {
            in.push_back(Pair("txid", txin.prevout.hash.GetHex()));
            in.push_back(Pair("vout", (int64_t)txin.prevout.n));
            UniValue o(UniValue::VOBJ);
            o.push_back(Pair("asm", txin.scriptSig.ToString()));
            o.push_back(Pair("hex", HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
            in.push_back(Pair("scriptSig", o));
        }
        in.push_back(Pair("sequence", (int64_t)txin.nSequence));
        vin.push_back(in);
    }
    entry.push_back(Pair("vin", vin));

    // add to entry obj the ceased sidechain withdrawal inputs
    Sidechain::AddCeasedSidechainWithdrawalInputsToJSON(tx, entry);

    UniValue vout(UniValue::VARR);
    for (unsigned int i = 0; i < tx.GetVout().size(); i++) {
        const CTxOut& txout = tx.GetVout()[i];
        UniValue out(UniValue::VOBJ);
        out.push_back(Pair("value", ValueFromAmount(txout.nValue)));
        out.push_back(Pair("valueZat", txout.nValue));
        out.push_back(Pair("n", (int64_t)i));
        UniValue o(UniValue::VOBJ);
        ScriptPubKeyToJSON(txout.scriptPubKey, o, true);
        out.push_back(Pair("scriptPubKey", o));
        vout.push_back(out);
    }
    entry.push_back(Pair("vout", vout));

    // add to entry obj the cross chain outputs
    Sidechain::AddSidechainOutsToJSON(tx, entry);

    UniValue vjoinsplit = TxJoinSplitToJSON(tx);
    entry.push_back(Pair("vjoinsplit", vjoinsplit));

    if (!hashBlock.IsNull()) {
        entry.push_back(Pair("blockhash", hashBlock.GetHex()));
        BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi != mapBlockIndex.end() && (*mi).second) {
            CBlockIndex* pindex = (*mi).second;
            if (chainActive.Contains(pindex)) {
                entry.push_back(Pair("confirmations", 1 + chainActive.Height() - pindex->nHeight));
                entry.push_back(Pair("time", pindex->GetBlockTime()));
                entry.push_back(Pair("blocktime", pindex->GetBlockTime()));
            }
            else
                entry.push_back(Pair("confirmations", 0));
        }
    }
}

void CertToJSON(const CScCertificate& cert, const uint256 hashBlock, UniValue& entry)
{
    entry.push_back(Pair("certid", cert.GetHash().GetHex()));
    entry.push_back(Pair("version", cert.nVersion));
    UniValue vin(UniValue::VARR);
    BOOST_FOREACH(const CTxIn& txin, cert.GetVin()) {
        UniValue in(UniValue::VOBJ);
        in.push_back(Pair("txid", txin.prevout.hash.GetHex()));
        in.push_back(Pair("vout", (int64_t)txin.prevout.n));
        UniValue o(UniValue::VOBJ);
        o.push_back(Pair("asm", txin.scriptSig.ToString()));
        o.push_back(Pair("hex", HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
        in.push_back(Pair("scriptSig", o));
        in.push_back(Pair("sequence", (int64_t)txin.nSequence));
        vin.push_back(in);
    }
    entry.push_back(Pair("vin", vin));
    UniValue vout(UniValue::VARR);
    for (unsigned int i = 0; i < cert.GetVout().size(); i++) {
        const CTxOut& txout = cert.GetVout()[i];
        UniValue out(UniValue::VOBJ);
        out.push_back(Pair("value", ValueFromAmount(txout.nValue)));
        out.push_back(Pair("valueZat", txout.nValue));
        out.push_back(Pair("n", (int64_t)i));
        UniValue o(UniValue::VOBJ);
        ScriptPubKeyToJSON(txout.scriptPubKey, o, true);
        out.push_back(Pair("scriptPubKey", o));
        if (cert.IsBackwardTransfer(i))
        {
            std::string pkhStr;
            auto it = std::find(txout.scriptPubKey.begin(), txout.scriptPubKey.end(), OP_HASH160);
            if (it != txout.scriptPubKey.end())
            {
                it += 2;
                std::vector<unsigned char> pkh(it, it + sizeof(uint160));
                pkhStr = HexStr(pkh.rbegin(), pkh.rend());
            }
            else
            {
                pkhStr = "<<Decode error>>";
            }
            out.push_back(Pair("backward transfer", true));
            out.push_back(Pair("pubkeyhash", pkhStr));
        }
        vout.push_back(out);
    }

    UniValue x(UniValue::VOBJ);
    x.push_back(Pair("scid", cert.GetScId().GetHex()));
    x.push_back(Pair("epochNumber", cert.epochNumber));
    x.push_back(Pair("quality", cert.quality));
    x.push_back(Pair("endEpochBlockHash", cert.endEpochBlockHash.GetHex()));
    x.push_back(Pair("endEpochCumScTxCommTreeRoot", cert.endEpochCumScTxCommTreeRoot.GetHexRepr()));
    x.push_back(Pair("scProof", HexStr(cert.scProof)));

    UniValue vCfe(UniValue::VARR);
    for (const auto& entry : cert.vFieldElementCertificateField) {
        vCfe.push_back(HexStr(entry.getVRawData()));
    }
    x.push_back(Pair("vFieldElementCertificateField", vCfe));

    UniValue vCmt(UniValue::VARR);
    for (const auto& entry : cert.vBitVectorCertificateField) {
        vCmt.push_back(HexStr(entry.getVRawData()));
    }
    x.push_back(Pair("vBitVectorCertificateField", vCmt));

    x.push_back(Pair("ftScFee", ValueFromAmount(cert.forwardTransferScFee)));
    x.push_back(Pair("mbtrScFee", ValueFromAmount(cert.mainchainBackwardTransferRequestScFee)));

    x.push_back(Pair("totalAmount", ValueFromAmount(cert.GetValueOfBackwardTransfers())));

    entry.push_back(Pair("cert", x));
    entry.push_back(Pair("vout", vout));

    if (!hashBlock.IsNull()) {
        entry.push_back(Pair("blockhash", hashBlock.GetHex()));
        BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi != mapBlockIndex.end() && (*mi).second) {
            CBlockIndex* pindex = (*mi).second;
            if (chainActive.Contains(pindex)) {
                entry.push_back(Pair("confirmations", 1 + chainActive.Height() - pindex->nHeight));
                entry.push_back(Pair("blocktime", pindex->GetBlockTime()));
            }
            else
            {
                entry.push_back(Pair("confirmations", 0));
            }
        }
    }
}

UniValue getrawtransaction(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getrawtransaction \"txid\" ( verbose )\n"
            "\nNOTE: By default this function only works sometimes. This is when the tx is in the mempool\n"
            "or there is an unspent output in the utxo for this transaction. To make it always work,\n"
            "you need to maintain a transaction index, using the -txindex command line option.\n"
            "\nReturn the raw transaction data.\n"
            "\nIf verbose=0, returns a string that is serialized, hex-encoded data for 'txid'.\n"
            "If verbose is non-zero, returns an Object with information about 'txid'.\n"

            "\nArguments:\n"
            "1. \"txid\"      (string, required) The transaction id\n"
            "2. verbose       (numeric, optional, default=0) If 0, return a string, other return a json object\n"

            "\nResult (if verbose is not set or set to 0):\n"
            "\"data\"      (string) The serialized, hex-encoded data for 'txid'\n"

            "\nResult (if verbose > 0):\n"
            "{\n"
            "  \"hex\" : \"data\",       (string) The serialized, hex-encoded data for 'txid'\n"
            "  \"txid\" : \"id\",        (string) The transaction id (same as provided)\n"
            "  \"version\" : n,          (numeric) The version\n"
            "  \"locktime\" : ttt,       (numeric) The lock time\n"
            "  \"vin\" : [               (array of json objects)\n"
            "     {\n"
            "       \"txid\": \"id\",    (string) The transaction id\n"
            "       \"vout\": n,         (numeric) \n"
            "       \"scriptSig\": {     (json object) The script\n"
            "         \"asm\": \"asm\",  (string) asm\n"
            "         \"hex\": \"hex\"   (string) hex\n"
            "       },\n"
            "       \"sequence\": n      (numeric) The script sequence number\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vcsw_ccin\" : [         (array of json objects) Ceased sidechain withdrawal inputs\n"
            "     {\n"
            "       \"value\": x.xxx,             (numeric) The value in " + CURRENCY_UNIT + "\n"
            "       \"scId\": \"hex\",            (string) The sidechain id\n"
            "       \"nullifier\": \"hex\",       (string) Withdrawal nullifier\n"
            "       \"scriptPubKey\" : {          (json object)\n"
            "         \"asm\" : \"asm\",          (string) the asm\n"
            "         \"hex\" : \"hex\",          (string) the hex\n"
            "         \"reqSigs\" : n,            (numeric) The required sigs\n"
            "         \"type\" : \"pubkeyhash\",  (string) The type, eg 'pubkeyhash'\n"
            "         \"addresses\" : [           (json array of string)\n"
            "           \"horizenaddress\"        (string) Horizen address\n"
            "           ,...\n"
            "         ]\n"
            "       },\n"
            "       \"scProof\": \"hex\",         (string) the zero-knowledge proof\n"
            "       \"redeemScript\": {           (json object) The script\n"
            "         \"asm\": \"asm\",           (string) asm\n"
            "         \"hex\": \"hex\"            (string) hex\n"
            "       }\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vout\" : [              (array of json objects)\n"
            "     {\n"
            "       \"value\" : x.xxx,            (numeric) The value in " + CURRENCY_UNIT + "\n"
            "       \"n\" : n,                    (numeric) index\n"
            "       \"scriptPubKey\" : {          (json object)\n"
            "         \"asm\" : \"asm\",          (string) the asm\n"
            "         \"hex\" : \"hex\",          (string) the hex\n"
            "         \"reqSigs\" : n,            (numeric) The required sigs\n"
            "         \"type\" : \"pubkeyhash\",  (string) The type, eg 'pubkeyhash'\n"
            "         \"addresses\" : [           (json array of string)\n"
            "           \"horizenaddress\"          (string) Horizen address\n"
            "           ,...\n"
            "         ]\n"
            "       }\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vsc_ccout\" : [           (array of json objects) Sidechain creation crosschain outputs\n"
            "     {\n"
            "       \"scid\" : \"hex\",                 (string) The sidechain id\n"
            "       \"n\" : n,                          (numeric) crosschain output index\n"
            "       \"withdrawal epoch length\" : n,    (numeric) Sidechain withdrawal epoch length\n"
            "       \"value\" : x.xxx,                  (numeric) The value in " + CURRENCY_UNIT + "\n"
            "       \"address\" : \"hex\",              (string) The sidechain receiver address\n"
            "       \"wCertVk\" : \"hex\",              (string) The sidechain certificate snark proof verification key\n"
            "       \"customData\" : \"hex\",           (string) The sidechain declaration custom data\n"
            "       \"constant\" : \"hex\",             (string) The sidechain certificate snark proof constant data\n"
            "       \"wCeasedVk\" : \"hex\",            (string, optional) The ceased sidechain withdrawal input snark proof verification key\n"
            "       \"ftScFee\" : n,                    (numeric) The fee in " + CURRENCY_UNIT + " required to create a Forward Transfer to sidechain\n"
            "       \"mbtrScFee\" : n,                  (numeric) The fee in " + CURRENCY_UNIT + " required to create a Mainchain Backward Transfer Request to sidechain\n"
            "       \"mbtrRequestDataLength\" : n       (numeric) The size of the MBTR request data length\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vft_ccout\" : [           (array of json objects) Sidechain forward transfer crosschain outputs\n"
            "     {\n"
            "       \"scid\" : \"hex\",           (string) The sidechain id\n"
            "       \"value\" : x.xxx,            (numeric) The value in " + CURRENCY_UNIT + "\n"
            "       \"n\" : n,                    (numeric) crosschain output index\n"
            "       \"address\" : \"hex\"         (string) The sidechain receiver address\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vjoinsplit\" : [        (array of json objects, only for version >= 2)\n"
            "     {\n"
            "       \"vpub_old\" : x.xxx,         (numeric) public input value in " + CURRENCY_UNIT + "\n"
            "       \"vpub_new\" : x.xxx,         (numeric) public output value in " + CURRENCY_UNIT + "\n"
            "       \"anchor\" : \"hex\",         (string) the anchor\n"
            "       \"nullifiers\" : [            (json array of string)\n"
            "         \"hex\"                     (string) input note nullifier\n"
            "         ,...\n"
            "       ],\n"
            "       \"commitments\" : [           (json array of string)\n"
            "         \"hex\"                     (string) output note commitment\n"
            "         ,...\n"
            "       ],\n"
            "       \"onetimePubKey\" : \"hex\",  (string) the onetime public key used to encrypt the ciphertexts\n"
            "       \"randomSeed\" : \"hex\",     (string) the random seed\n"
            "       \"macs\" : [                  (json array of string)\n"
            "         \"hex\"                     (string) input note MAC\n"
            "         ,...\n"
            "       ],\n"
            "       \"proof\" : \"hex\",          (string) the zero-knowledge proof\n"
            "       \"ciphertexts\" : [           (json array of string)\n"
            "         \"hex\"                     (string) output note ciphertext\n"
            "         ,...\n"
            "       ]\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"blockhash\" : \"hash\",   (string) the block hash\n"
            "  \"confirmations\" : n,      (numeric) The confirmations\n"
            "  \"time\" : ttt,             (numeric) The transaction time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"blocktime\" : ttt         (numeric) The block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("getrawtransaction", "\"mytxid\"")
            + HelpExampleCli("getrawtransaction", "\"mytxid\" 1")
            + HelpExampleRpc("getrawtransaction", "\"mytxid\", 1")
        );
    LOCK(cs_main);

    uint256 hash = ParseHashV(params[0], "parameter 1");

    bool fVerbose = false;
    if (params.size() > 1)
        fVerbose = (params[1].get_int() != 0);

    // allocated by the callee
    std::unique_ptr<CTransactionBase> pTxBase;
    
    uint256 hashBlock{};
    if (!GetTxBaseObj(hash, pTxBase, hashBlock, true) || !pTxBase)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available about transaction");

    std::string strHex = EncodeHex(pTxBase);

    if (!fVerbose) {
        return strHex;
    }

    UniValue result(UniValue::VOBJ);
    try {
        if (pTxBase->IsCertificate()) {
            CScCertificate cert(dynamic_cast<const CScCertificate&>(*pTxBase));
            CertToJSON(cert, hashBlock, result);
        } else {
            CTransaction tx(dynamic_cast<const CTransaction&>(*pTxBase));
            TxToJSON(tx, hashBlock, result);
        }
    } catch (std::exception& e) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, std::string("internal error: ") + std::string(e.what() ));
    }
    
    result.push_back(Pair("hex", strHex));
    return result;
}

UniValue getrawcertificate(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getrawcertificate \"certid\" ( verbose )\n"
            "\nNOTE: By default this function only works sometimes. This is when the certificate is in the mempool\n"
            "or there is an unspent output in the utxo for this certificate. To make it always work,\n"
            "you need to maintain a transaction index, using the -txindex command line option.\n"
            "\nReturn the raw certificate data.\n"
            "\nIf verbose=0, returns a string that is serialized, hex-encoded data for 'certid'.\n"
            "If verbose is non-zero, returns an Object with information about 'certid'.\n"

            "\nArguments:\n"
            "1. \"certid\"      (string, required) The certificate id\n"
            "2. verbose       (numeric, optional, default=0) If 0, return a string, other return a json object\n"

            "\nResult (if verbose is not set or set to 0):\n"
            "\"data\"      (string) The serialized, hex-encoded data for 'certid'\n"

            "\nResult (if verbose > 0):\n"
            "{\n"
            "  \"hex\" : \"data\",         (string) The serialized, hex-encoded data for 'certid'\n"
            "  \"certid\" : \"id\",        (string) The transaction id (same as provided)\n"
            "  \"version\" : n,          (numeric) The version\n"
            "  \"cert\" :                (json object)\n"
            "     {\n"
            "       \"scid\" : \"sc id\",                      (string) the sidechain id\n"
            "       \"epochNumber\": epn,                      (numeric) the withdrawal epoch number this certificate refers to\n"
            "       \"quality\": n,                            (numeric) the quality of this withdrawal certificate. \n"
            "       \"endEpochBlockHash\" : \"eph\"            (string) the hash of the block marking the end of the abovementioned epoch\n"
            "       \"endEpochCumScTxCommTreeRoot\" : \"ecum\" (string) The hex string representation of the field element corresponding to the root of the cumulative scTxCommitment tree stored at the block marking the end of the referenced epoch\n"
            "       \"scProof\": \"scp\"                       (string) SNARK proof whose verification key wCertVk was set upon sidechain registration\n"
            "       \"totalAmount\" : x.xxx                    (numeric) The total value of the certificate in " + CURRENCY_UNIT + "\n"
            "     }\n"
            "  \"vout\" : [              (array of json objects)\n"
            "     {\n"
            "       \"value\" : x.xxx,            (numeric) The value in " + CURRENCY_UNIT + "\n"
            "       \"valueZat\" : xxxx,          (numeric) The value in Zat\n"
            "       \"n\" : n,                    (numeric) index\n"
            "       \"scriptPubKey\" : {          (json object)\n"
            "         \"asm\" : \"asm\",            (string) the asm\n"
            "         \"hex\" : \"hex\",            (string) the hex\n"
            "         \"type\" : \"pubkeyhash\",    (string) The type, eg 'pubkeyhash'\n"
            "         \"addresses\" : [           (json array of string)\n"
            "           \"horizenaddress\"        (string) Horizen address\n"
            "           ,...\n"
            "         ]\n"
            "       }\n"
            "       --- optional fields present only if this vout is a backward transfer:\n" 
            "       \"backward transfer\" : true  (bool)\n" 
            "       \"pubkeyhash\" : \"pkh\"        (string) public key hash this backward transfer refers to, it corresponds to the horizen address specified above"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"blockhash\" : \"hash\",   (string) the block hash\n"
            "  \"confirmations\" : n,    (numeric) The confirmations\n"
            "  \"blocktime\" : ttt       (numeric) The block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("getrawcertificate", "\"mycertid\"")
            + HelpExampleCli("getrawcertificate", "\"mycertid\" 1")
            + HelpExampleRpc("getrawcertificate", "\"mycertid\", 1")
        );
    LOCK(cs_main);

    uint256 hash = ParseHashV(params[0], "parameter 1");

    bool fVerbose = false;
    if (params.size() > 1)
        fVerbose = (params[1].get_int() != 0);

    CScCertificate cert;
    uint256 hashBlock;
    if (!GetCertificate(hash, cert, hashBlock, true))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available about certificate");

    string strHex = EncodeHexCert(cert);

    if (!fVerbose)
        return strHex;

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("hex", strHex));
    CertToJSON(cert, hashBlock, result);
    return result;
}

UniValue gettxoutproof(const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() != 1 && params.size() != 2))
        throw runtime_error(
            "gettxoutproof [\"txid\",...] ( blockhash )\n"
            "\nReturns a hex-encoded proof that \"txid\" was included in a block.\n"
            "\nNOTE: By default this function only works sometimes. This is when there is an\n"
            "unspent output in the utxo for this transaction. To make it always work,\n"
            "you need to maintain a transaction index, using the -txindex command line option or\n"
            "specify the block in which the transaction is included in manually (by blockhash).\n"
            "\nReturn the raw transaction data.\n"
            "\nArguments:\n"
            "1. \"txids\"       (string) A json array of txids to filter\n"
            "    [\n"
            "      \"txid\"     (string) A transaction hash\n"
            "      ,...\n"
            "    ]\n"
            "2. \"block hash\"  (string, optional) If specified, looks for txid in the block with this hash\n"
            "\nResult:\n"
            "\"data\"           (string) A string that is a serialized, hex-encoded data for the proof.\n"
        );

    set<uint256> setTxids;
    uint256 oneTxid;
    UniValue txids = params[0].get_array();
    for (size_t idx = 0; idx < txids.size(); idx++) {
        const UniValue& txid = txids[idx];
        if (txid.get_str().length() != 64 || !IsHex(txid.get_str()))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid txid ")+txid.get_str());
        uint256 hash(uint256S(txid.get_str()));
        if (setTxids.count(hash))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, duplicated txid: ")+txid.get_str());
       setTxids.insert(hash);
       oneTxid = hash;
    }

    LOCK(cs_main);

    CBlockIndex* pblockindex = NULL;

    uint256 hashBlock;
    if (params.size() > 1)
    {
        hashBlock = uint256S(params[1].get_str());
        if (!mapBlockIndex.count(hashBlock))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        pblockindex = mapBlockIndex[hashBlock];
    } else {
        CCoins coins;
        if (pcoinsTip->GetCoins(oneTxid, coins) && coins.nHeight > 0 && coins.nHeight <= chainActive.Height())
            pblockindex = chainActive[coins.nHeight];
    }

    if (pblockindex == NULL)
    {
        CTransaction tx;
        if (!GetTransaction(oneTxid, tx, hashBlock, false) || hashBlock.IsNull())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not yet in block");
        if (!mapBlockIndex.count(hashBlock))
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Transaction index corrupt");
        pblockindex = mapBlockIndex[hashBlock];
    }

    CBlock block;
    if(!ReadBlockFromDisk(block, pblockindex))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Can't read block from disk");

    unsigned int ntxFound = 0;
    BOOST_FOREACH(const CTransaction&tx, block.vtx)
        if (setTxids.count(tx.GetHash()))
            ntxFound++;
    if (ntxFound != setTxids.size())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "(Not all) transactions not found in specified block");

    CDataStream ssMB(SER_NETWORK, PROTOCOL_VERSION);
    CMerkleBlock mb(block, setTxids);
    ssMB << mb;
    std::string strHex = HexStr(ssMB.begin(), ssMB.end());
    return strHex;
}

UniValue verifytxoutproof(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "verifytxoutproof \"proof\"\n"
            "\nVerifies that a proof points to a transaction in a block, returning the transaction it commits to\n"
            "and throwing an RPC error if the block is not in our best chain\n"
            "\nArguments:\n"
            "1. \"proof\"    (string, required) The hex-encoded proof generated by gettxoutproof\n"
            "\nResult:\n"
            "[\"txid\"]      (array, strings) The txid(s) which the proof commits to, or empty array if the proof is invalid\n"
        );

    CDataStream ssMB(ParseHexV(params[0], "proof"), SER_NETWORK, PROTOCOL_VERSION);
    CMerkleBlock merkleBlock;
    ssMB >> merkleBlock;

    UniValue res(UniValue::VARR);

    vector<uint256> vMatch;
    if (merkleBlock.txn.ExtractMatches(vMatch) != merkleBlock.header.hashMerkleRoot)
        return res;

    LOCK(cs_main);

    if (!mapBlockIndex.count(merkleBlock.header.GetHash()) || !chainActive.Contains(mapBlockIndex[merkleBlock.header.GetHash()]))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found in chain");

    BOOST_FOREACH(const uint256& hash, vMatch)
        res.push_back(hash.GetHex());
    return res;
}

void AddInputsToRawObject(CMutableTransactionBase& rawTxObj, const UniValue& inputs)
{
    // inputs
    for (size_t idx = 0; idx < inputs.size(); idx++) {
        const UniValue& input = inputs[idx];
        const UniValue& o = input.get_obj();

        uint256 txid = ParseHashO(o, "txid");

        const UniValue& vout_v = find_value(o, "vout");
        if (!vout_v.isNum())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, missing vout key");
        int nOutput = vout_v.get_int();
        if (nOutput < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout must be positive");

        CTxIn in(COutPoint(txid, nOutput));
        rawTxObj.vin.push_back(in);
    }
}

void AddOutputsToRawObject(CMutableTransactionBase& rawTxObj, const UniValue& sendTo)
{
    set<CBitcoinAddress> setAddress;
    vector<string> addrList = sendTo.getKeys();
    BOOST_FOREACH(const string& name_, addrList) {
        CBitcoinAddress address(name_);
        if (!address.IsValid())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("Invalid Horizen address: ")+name_);

        if (setAddress.count(address))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, duplicated address: ")+name_);
        setAddress.insert(address);

        CScript scriptPubKey = GetScriptForDestination(address.Get());
        CAmount nAmount = AmountFromValue(sendTo[name_]);

        rawTxObj.addOut(CTxOut(nAmount, scriptPubKey));
    }
}

UniValue createrawtransaction(const UniValue& params, bool fHelp)
{   
    if (fHelp || params.size() > 6)
        throw runtime_error(
            "createrawtransaction [{\"txid\":\"id\",\"vout\":n},...] {\"address\":amount,...} (\n"
            "    [{\"amount\": value, \"senderAddress\":\"address\", ...}, ...] (\n"
            "    [{\"epoch_length\":h, \"address\":\"address\", \"amount\":amount, \"wCertVk\":hexstr, \"customData\":[hexstr1, hextstr2, ...], \"constant\":hexstr},...]\n"
            "    ( [{\"address\":\"address\", \"amount\":amount, \"scid\":id},...]\n"
            "    ( [{\"scid\":\"scid\", \"vScRequestData\":\"vScRequestData\", \"pubkeyhash\":\"pubkeyhash\", \"scFee\":\"scFee\", \"scProof\":\"scProof\"},...]\n"
            ") ) )\n"
            "\nCreate a transaction spending the given inputs and sending to the given addresses.\n"
            "Returns hex-encoded raw transaction.\n"
            "Note that the transaction's inputs are not signed, and\n"
            "it is not stored in the wallet or transmitted to the network.\n"
            "See also \"fundrawtransaction\" RPC method.\n"

            "\nArguments:\n"
            "1. \"transactions\"        (string, required) A json array of json objects\n"
            "     [\n"
            "       {\n"
            "         \"txid\":\"id\",  (string, required) The transaction id\n"
            "         \"vout\":n        (numeric, required) The output number\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"
            "2. \"addresses\"           (string, required) a json object with addresses as keys and amounts as values\n"
            "    {\n"
            "      \"address\": x.xxx   (numeric, required) The key is the Horizen address, the value is the " + CURRENCY_UNIT + " amount\n"
            "      ,...\n"
            "    }\n"
            "3. \"ceased sidechain withdrawal inputs\"      (string, optional but required if 4 and 5 are also given) A json array of json objects\n"
            "     [\n"
            "       {\n"
            "         \"amount\": x.xxx,                   (numeric, required) The numeric amount in " + CURRENCY_UNIT + " is the value\n"
            "         \"senderAddress\": \"address\",      (string, required) The sender Horizen address\n"
            "         \"scId\": \"hex\",                   (string, required) The ceased sidechain id\n"
            "         \"nullifier\": \"hex\",              (string, required) Withdrawal nullifier\n"
            "         \"scProof\": \"hex\"                 (string, required) SNARK proof whose verification key was set upon sidechain registration. Its size must be " + strprintf("%d", SC_PROOF_SIZE) + "bytes \n"
            "         \"activeCertData\": \"hex\",         (string, required) Active Certificate Data Hash\n"
            "         \"ceasingCumScTxCommTree\": \"hex\", (string, required) Cumulative SC Committment tree hash of the ceasing block\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"
            "4. \"sc creations\"        (string, optional but required if 5 is also given) A json array of json objects\n"
            "     [\n"
            "       {\n"
            "         \"epoch_length\":n (numeric, required) length of the withdrawal epochs\n"
            "         \"address\":\"address\",  (string, required) The receiver PublicKey25519Proposition in the SC\n"
            "         \"amount\":amount         (numeric, required) The numeric amount in " + CURRENCY_UNIT + " is the value\n"
            "         \"wCertVk\":hexstr          (string, required) It is an arbitrary byte string of even length expressed in\n"
            "                                       hexadecimal format. Required to verify a WCert SC proof. Its size must be " + strprintf("%d", SC_VK_SIZE) + " bytes\n"
            "         \"customData\":hexstr       (string, optional) It is an arbitrary byte string of even length expressed in\n"
            "                                       hexadecimal format. A max limit of " + strprintf("%d", Sidechain::MAX_SC_CUSTOM_DATA_LEN) + " bytes will be checked\n"
            "         \"constant\":hexstr         (string, optional) It is an arbitrary byte string of even length expressed in\n"
            "                                       hexadecimal format. Used as public input for WCert proof verification. Its size must be " + strprintf("%d", CFieldElement::ByteSize()) + " bytes\n"
            "         \"wCeasedVk\":hexstr        (string, optional) It is an arbitrary byte string of even length expressed in\n"
            "                                       hexadecimal format. Used to verify a Ceased sidechain withdrawal proofs for given SC. Its size must be " + strprintf("%d", SC_VK_SIZE) + " bytes\n"
            "         \"vFieldElementCertificateFieldConfig\" (array, optional) An array whose entries are sizes (in bits). Any certificate should have as many FieldElementCertificateField with the corresponding size.\n"
            "         \"vBitVectorCertificateFieldConfig\"    (array, optional) An array whose entries are bitVectorSizeBits and maxCompressedSizeBytes pairs. Any certificate should have as many BitVectorCertificateField with the corresponding sizes\n"
            "         \"forwardTransferScFee\" (numeric, optional, default=0) The amount of fee in " + CURRENCY_UNIT + " due to sidechain actors when creating a FT\n"
            "         \"mainchainBackwardTransferScFee\" (numeric, optional, default=0) The amount of fee in " + CURRENCY_UNIT + " due to sidechain actors when creating a MBTR\n"
            "         \"mainchainBackwardTransferRequestDataLength\" (numeric, optional, default=0) The expected size (max=" + strprintf("%d", Sidechain::MAX_SC_MBTR_DATA_LEN) + ") of the request data vector (made of field elements) in a MBTR\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"
            "5. \"forward transfers\"   (string, optional) A json array of json objects\n"
            "     [\n"
            "       {\n"
            "         \"address\":\"address\",  (string, required) The receiver PublicKey25519Proposition in the SC\n"
            "         \"amount\":amount         (numeric, required) The numeric amount in " + CURRENCY_UNIT + " is the value to transfer to SC\n"
            "         \"scid\":side chain ID    (string, required) The uint256 side chain ID\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"
            "6. \"backward transfer requests\"   (string, optional) A json array of json objects\n"
            "     [\n"
            "       {\n"
            "         \"scid\":side chain ID       (string, required) The uint256 side chain ID\n"
            "         \"vScRequestData\":           (array, required) It is an arbitrary array of byte strings of even length expressed in\n"
            "                                         hexadecimal format representing the SC Utxo ID for which a backward transafer is being requested. Its size must be " + strprintf("%d", CFieldElement::ByteSize()) + " bytes\n"
            "         \"pubkeyhash\":pkh           (string, required) The uint160 public key hash corresponding to a main chain address where to send the backward transferred amount\n"
            "         \"scFee\":amount,            (numeric, required) The numeric amount in " + CURRENCY_UNIT + " representing the value spent by the sender that will be gained by a SC forger\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"

            "\nResult:\n"
            "\"transaction\"            (string) hex string of the transaction\n"

            "\nExamples\n"
            + HelpExampleCli("createrawtransaction", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\" \"{\\\"address\\\":0.01}\"")
            + HelpExampleCli("createrawtransaction", "\"[]\" \"{}\" \"[]\" \"[{\\\"forwardTransferScFee\\\": 10.0, \\\"epoch_length\\\": 10, \\\"wCertVk\\\": \\\"4157d96790cc632ef7c1b89d17bb54c687ad90527f4f650022b0f499b734d1e66e46dbe1bc834488d80c6d4e495270f51db75edc65ad77becb4f535f5678ee27adefcd903a1fb93f33c98d51a3e1959f4f02c85b3384c7e5c658e758e8a00100620e7540fd80b9df71a72fe7a1fc0e12e1b6d1503b052757f40383628cd14c0f9777240e882f55aba752312767022c02adaf7a1758be03e2eb51cfdb0ee7cb3490c58082225e52229961c8f3ba31e182e1c216473c7ba163471ce341efa7000053b3d397ac75f93c27a3660584b5378e9386bb9d6b8a5ba60a4f0d66512a323b77a4ae29746c00a96e2fdd7b31f10b0a4b13becd0323eeed07904f4c3e31cf3c08df04086216b9826fc3baac6eb64ed3cf9598001311d081fdeb2c0232d80000b5f2f0874f5d8ec899c5b5299ca829c1ea7f1a4838d6f5fb41dd7b866237e786cc38311f5e148db69881fd066bfb626d400ac6abb43f30fcfe159afc52a269027028cbc5cb160e273ba1be9d7bd493dcd9b5911d14008f42ec9b39af2c8d0000b749ca5a4a21a6a49ec2c4e7dfa13d694fb08d9419220919989ca578e072305104483251543dcb4266161d90f3d3705065eed9352c581d5138380ad88eaf28cefa2a76b263208ad6357a544b66f96e82d348d34fc726e6bcc6bb127dd4330100a0347993307c563c5ac0e2188dc9a0e3205fcd709db15539e3d885b615f68d475a7cde28b35448851bca51875364c696bfdeb91ae1aad14238b397bb7d66c5c4a14703b3d93fa36ada62f92149ccd055c8b4801cb2be3869fd6cc79a188b000052d447cddcfdf23b64f4f557ac5323b09cba9b99028d051e97aa4f520fd94b2714a50aba22a53c1d7eebe8c80288bedccf05ebb4a615420d87b227904126117418d031608a92b92c59a40949c496680924acf61d18570dc83dbf00b87a6b010022a39355eb55b963221190e140d39362796cf3a2a906ef4d76288c406a90a31e0cf6010c3ca36d2b38139e800cf4e5094ab119290e64456b620b8d01b384ebca3cb04d168704b82af61a7b67fd6cc78f280d24a685571b55b1d994948a3801000070ddb8512cad5aadc7acceae7735f6de32efc2576263b48feeeeaaa430bce6df377bf73a0354eab5b098f103cfe3dcf17c904ab9d31d62bb541fa10cad6a9551c628c3bcda726bba05d53696cadf2ea49a158d0e20a5272ea2c6cd72b6cc0000fe8e46678a8aff3c3652bac7f4cb63e85e5871259da4d025ba7f7f565e00c8a6044b840cc5b5d01980484caa4738e80529d19c57ff5a52187083539e335d2db8642cdf4080ae31d60eea4171431962046261adccc67e58a279a29e733a5500000eb15b45f67a258f8e535667fb267d59102df8822d5307458543f14f7d0ac2cbfa065811d4391457d3bff5c08d38a506bcacfb8684538a5c80514e6734c5c235c208a4cd9596dd6bb354c30fe298a5af7e0a766fd8a8c2a1394b6be2a1470100b17623e1781dcf8221a773b2cf80402306b9ec7e5b67e0e4fe35445e9a8f287108a133e7f9d99b5552886a524ebc104855dc2d9ed5e9deb48c1daf27be4fdd5b6515d6147eb618f2d2ff1c15bf2e6b6bafe76ae82535d721eae3bd6fb2b400000000000002280eebcc8685997d6f3fc30e8199fb8a0d80948427d2030dad55aba0f04f821c9d6e59436f83b9d89c3b38a701a65b11f764655482cdc4506df9f5156dd31d23adcdbb70de819a70958e8c4ad9372934451e6587dd3fae6e63ea4bffffa801009115852ce3a295b22c054fbd779f387f89dee0f498b43d272db7b3ebcd0eb070b791aa771a14e3830784bcc1bc6df7b82d9c0fbc4c93ebe187445b4687464ada2ff7db60f9e8783b800974b54bbae4305344f48eb8c370c9d96790e000960000007ccc374fffbfb4bc5d7385e695d6462e2a94a125977fabc4c6d2d2071bde65a249f7b7191e53e8a96a6f758d6395652eeaef56b6cea6845f7e6eef492b6fe87b7aef7c084f549744349ce3a05e8bb21791d765fd91359d8a703c49d2331901008898e992dc633488016a1576ca471eabbfac0f8fd2589d3be087f9cae89dc842a270edd2cb7e787690ee542b3cb8cc17e69aa769afaa8e8d830e7a0b4277354299506ec49ef4a2ebf2c15011be320acf2e19dabbf50268c47441c0406ab4010000\\\", \\\"constant\\\": \\\"07c71a9b7880be136ad0871715b51bfecd953f498c5b5b115a5e9983f2e22b0398aedf38cdbbee9e1fa4a54c16a40ac87dd7bd337d15ffb06307d0f6f0e6352cd11621e967f17b25c1a61834598c7914f1e11a3237617179c92ee31e78ee0000\\\", \\\"address\\\": \\\"dada\\\", \\\"vFieldElementCertificateFieldConfig\\\": [], \\\"mainchainBackwardTransferRequestDataLength\\\": 1, \\\"vBitVectorCertificateFieldConfig\\\": [], \\\"mainchainBackwardTransferScFee\\\": 20.0, \\\"amount\\\": 50.0}]\"")
            + HelpExampleRpc("createrawtransaction", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\", \"{\\\"address\\\":0.01}\"")
            + HelpExampleRpc("createrawtransaction", "\"[]\", \"{\\\"address\\\":0.01}\" \"[{\\\"amount\\\": 0.02, \\\"scId\\\": \\\"myscid\\\", \\\"nullifier\\\": \\\"mynullifier\\\", \\\"scProof\\\": \\\"proof\\\"}]\"")
            + HelpExampleRpc("createrawtransaction", "\"[]\" \"{}\" \"[{\\\"epoch_length\\\" :300}]\" \"{\\\"address\\\": \\\"myaddress\\\", \\\"amount\\\": 4.0, \\\"scid\\\": \\\"myscid\\\"}]\"")
        );

    LOCK(cs_main);
    RPCTypeCheck(params, boost::assign::list_of 
        (UniValue::VARR)(UniValue::VOBJ)(UniValue::VARR)(UniValue::VARR)(UniValue::VARR)(UniValue::VARR));

    UniValue inputs = params[0].get_array();
    UniValue sendTo = params[1].get_obj();

    CMutableTransaction rawTx;

    AddInputsToRawObject(rawTx, inputs);
    AddOutputsToRawObject(rawTx, sendTo);

    // ceased sidechain withdrawal inputs
    if (params.size() > 2 && !params[2].isNull())
    {
        UniValue csws = params[2].get_array();

        if (csws.size())
        {
            std::string errString;
            if (!Sidechain::AddCeasedSidechainWithdrawalInputs(csws, rawTx, errString) )
            {
                throw JSONRPCError(RPC_TYPE_ERROR, errString);
            }
        }
    }

    // crosschain creation
    if (params.size() > 3 && !params[3].isNull())
    {
        UniValue sc_crs = params[3].get_array();

        if (sc_crs.size())
        {
            std::string errString;
            if (!Sidechain::AddSidechainCreationOutputs(sc_crs, rawTx, errString) )
            {
                throw JSONRPCError(RPC_TYPE_ERROR, errString);
            }
        }
    }

    // crosschain forward transfers
    if (params.size() > 4 && !params[4].isNull())
    {
        UniValue fwdtr = params[4].get_array();

        if (fwdtr.size())
        {
            std::string errString;
            if (!Sidechain::AddSidechainForwardOutputs(fwdtr, rawTx, errString) )
            {
                throw JSONRPCError(RPC_TYPE_ERROR, errString);
            }
        }
    }

    // bwt requests 
    if (params.size() > 5 && !params[5].isNull())
    {
        UniValue bwtreq = params[5].get_array();

        if (bwtreq.size())
        {
            std::string errString;
            if (!Sidechain::AddSidechainBwtRequestOutputs(bwtreq, rawTx, errString) )
            {
                throw JSONRPCError(RPC_TYPE_ERROR, errString);
            }
        }
    }


    return EncodeHexTx(rawTx);
}

UniValue decoderawtransaction(const UniValue& params, bool fHelp)
{   
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "decoderawtransaction \"hexstring\"\n"
            "\nReturn a JSON object representing the serialized, hex-encoded transaction.\n"

            "\nArguments:\n"
            "1. \"hex\"      (string, required) The transaction hex string\n"

            "\nResult:\n"
            "{\n"
            "  \"txid\" : \"id\",        (string) The transaction id\n"
            "  \"version\" : n,          (numeric) The version\n"
            "  \"locktime\" : ttt,       (numeric) The lock time\n"
            "  \"vin\" : [               (array of json objects)\n"
            "     {\n"
            "       \"txid\": \"id\",    (string) The transaction id\n"
            "       \"vout\": n,         (numeric) The output number\n"
            "       \"scriptSig\": {     (json object) The script\n"
            "         \"asm\": \"asm\",  (string) asm\n"
            "         \"hex\": \"hex\"   (string) hex\n"
            "       },\n"
            "       \"sequence\": n     (numeric) The script sequence number\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vcsw_ccin\" : [         (array of json objects) Ceased sidechain withdrawal inputs\n"
            "     {\n"
            "       \"value\": x.xxx,             (numeric) The value in " + CURRENCY_UNIT + "\n"
            "       \"scId\": \"hex\",            (string) The sidechain id\n"
            "       \"nullifier\": \"hex\",       (string) Withdrawal nullifier\n"
            "       \"scriptPubKey\" : {          (json object)\n"
            "         \"asm\" : \"asm\",          (string) the asm\n"
            "         \"hex\" : \"hex\",          (string) the hex\n"
            "         \"reqSigs\" : n,            (numeric) The required sigs\n"
            "         \"type\" : \"pubkeyhash\",  (string) The type, eg 'pubkeyhash'\n"
            "         \"addresses\" : [           (json array of string)\n"
            "           \"horizenaddress\"        (string) Horizen address\n"
            "           ,...\n"
            "         ]\n"
            "       },\n"
            "       \"scProof\": \"hex\",         (string) the zero-knowledge proof\n"
            "       \"redeemScript\": {           (json object) The script\n"
            "         \"asm\": \"asm\",           (string) asm\n"
            "         \"hex\": \"hex\"            (string) hex\n"
            "       }\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vout\" : [             (array of json objects)\n"
            "     {\n"
            "       \"value\" : x.xxx,            (numeric) The value in " + CURRENCY_UNIT + "\n"
            "       \"n\" : n,                    (numeric) index\n"
            "       \"scriptPubKey\" : {          (json object)\n"
            "         \"asm\" : \"asm\",          (string) the asm\n"
            "         \"hex\" : \"hex\",          (string) the hex\n"
            "         \"reqSigs\" : n,            (numeric) The required sigs\n"
            "         \"type\" : \"pubkeyhash\",  (string) The type, eg 'pubkeyhash'\n"
            "         \"addresses\" : [           (json array of string)\n"
            "           \"t12tvKAXCxZjSmdNbao16dKXC8tRWfcF5oc\"   (string) Horizen address\n"
            "           ,...\n"
            "         ]\n"
            "       }\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vsc_ccout\" : [           (array of json objects) Sidechain creation crosschain outputs\n"
            "     {\n"
            "       \"scid\" : \"hex\",                 (string) The sidechain id\n"
            "       \"n\" : n,                          (numeric) crosschain output index\n"
            "       \"withdrawal epoch length\" : n,    (numeric) Sidechain withdrawal epoch length\n"
            "       \"value\" : x.xxx,                  (numeric) The value in " + CURRENCY_UNIT + "\n"
            "       \"address\" : \"hex\",              (string) The sidechain receiver address\n"
            "       \"wCertVk\" : \"hex\",              (string) The sidechain certificate snark proof verification key\n"
            "       \"customData\" : \"hex\",           (string) The sidechain declaration custom data\n"
            "       \"constant\" : \"hex\",             (string) The sidechain certificate snark proof constant data\n"
            "       \"wCeasedVk\" : \"hex\"             (string, optional) The ceased sidechain withdrawal input snark proof verification key\n"
            "       \"ftScFee\" :                       (numeric) The fee in " + CURRENCY_UNIT + " required to create a Forward Transfer to sidechain\n"
            "       \"mbtrScFee\"                       (numeric) The fee in " + CURRENCY_UNIT + " required to create a Mainchain Backward Transfer Request to sidechain\n"
            "       \"mbtrRequestDataLength\"           (numeric) The size of the MBTR request data length\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vft_ccout\" : [           (array of json objects) Sidechain forward transfer crosschain outputs\n"
            "     {\n"
            "       \"scid\" : \"hex\",           (string) The sidechain id\n"
            "       \"value\" : x.xxx,            (numeric) The value in " + CURRENCY_UNIT + "\n"
            "       \"n\" : n,                    (numeric) crosschain output index\n"
            "       \"address\" : \"hex\"         (string) The sidechain receiver address\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vjoinsplit\" : [        (array of json objects, only for version >= 2)\n"
            "     {\n"
            "       \"vpub_old\" : x.xxx,         (numeric) public input value in " + CURRENCY_UNIT + "\n"
            "       \"vpub_new\" : x.xxx,         (numeric) public output value in " + CURRENCY_UNIT + "\n"
            "       \"anchor\" : \"hex\",         (string) the anchor\n"
            "       \"nullifiers\" : [            (json array of string)\n"
            "         \"hex\"                     (string) input note nullifier\n"
            "         ,...\n"
            "       ],\n"
            "       \"commitments\" : [           (json array of string)\n"
            "         \"hex\"                     (string) output note commitment\n"
            "         ,...\n"
            "       ],\n"
            "       \"onetimePubKey\" : \"hex\",  (string) the onetime public key used to encrypt the ciphertexts\n"
            "       \"randomSeed\" : \"hex\",     (string) the random seed\n"
            "       \"macs\" : [                  (json array of string)\n"
            "         \"hex\"                     (string) input note MAC\n"
            "         ,...\n"
            "       ],\n"
            "       \"proof\" : \"hex\",          (string) the zero-knowledge proof\n"
            "       \"ciphertexts\" : [           (json array of string)\n"
            "         \"hex\"                     (string) output note ciphertext\n"
            "         ,...\n"
            "       ]\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("decoderawtransaction", "\"hexstring\"")
            + HelpExampleRpc("decoderawtransaction", "\"hexstring\"")
        );

    LOCK(cs_main);
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR));

    // allocated by the callee
    std::unique_ptr<CTransactionBase> pTxBase;

    if (!DecodeHex(pTxBase, params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");

    UniValue result(UniValue::VOBJ);
    try {
        if (pTxBase->IsCertificate()) {
            CScCertificate cert(dynamic_cast<const CScCertificate&>(*pTxBase));
            CertToJSON(cert, uint256(), result);
        } else {
            CTransaction tx(dynamic_cast<const CTransaction&>(*pTxBase));
            TxToJSON(tx, uint256(), result);
        }
    } catch (std::exception& e) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, std::string("internal error: ") + std::string(e.what() ));
    }

    return result;
}

UniValue createrawcertificate(const UniValue& params, bool fHelp)
{   
    if (fHelp || params.size() != 4)
        throw runtime_error(
            "createrawcertificate [{\"txid\":\"id\",\"vout\":n},...] {\"address\":amount,...} {\"pubkeyhash\":amount,...} {\"scid\":\"id\", \"withdrawalEpochNumber\":n, \"quality\":n, \"endEpochBlockHash\":\"blockHash\", \"endEpochCumScTxCommTreeRoot\":\"cum\", \"scProof\":\"scProof\"})\n"
            "\nCreate a SC certificate spending the given inputs, sending to the given addresses and transferring funds from the specified SC to the given pubkey hash list.\n"
            "Returns hex-encoded raw certificate.\n"
            "It is not stored in the wallet or transmitted to the network.\n"

            "\nArguments:\n"
            "1. \"transactions\"           (string, required) A json array of json objects. Can be an empty array\n"
            "     [\n"                     
            "       {\n"                   
            "         \"txid\":\"id\",                 (string, required) The transaction id\n"
            "         \"vout\":n                     (numeric, required) The output number\n"
            "       }\n"                   
            "       ,...\n"                
            "     ]\n"                     
            "2. \"vout addresses\"         (string, required) a json object with addresses as keys and amounts as values. Can also be an empty obj\n"
            "    {\n"                      
            "      \"address\": x.xxx                (numeric, required) The key is the Horizen address, the value is the " + CURRENCY_UNIT + " amount\n"
            "      ,...\n"                            
            "    }\n"                      
            "3. \"backward addresses\"     (string, required) A json object with pubkeyhash as keys and amounts as values. Can be an empty obj if no amounts are trasferred (empty certificate)\n"
            "    {\n"                               
            "      \"pubkeyhash\": x.xxx             (numeric, required) The public key hash corresponding to a Horizen address and the " + CURRENCY_UNIT + " amount to send to\n"
            "      ,...\n"                                  
            "    }\n"                               
            "4. \"certificate parameters\" (string, required) A json object with a list of key/values\n"
            "    {\n"
            "      \"scid\":\"id\",                    (string, required) The side chain id\n"
            "      \"withdrawalEpochNumber\":n       (numeric, required) The epoch number this certificate refers to\n"
            "      \"quality\":n                     (numeric, required) A positive number specifying the quality of this withdrawal certificate. \n"
            "      \"endEpochBlockHash\":\"blockHash\" (string, required) The block hash determining the end of the referenced epoch\n"
            "      \"endEpochCumScTxCommTreeRoot\":\"ecum\" (string, required) The hex string representation of the field element corresponding to the root of the cumulative scTxCommitment tree stored at the block marking the end of the referenced epoch\n"
            "      \"scProof\":\"scProof\"             (string, required) SNARK proof whose verification key wCertVk was set upon sidechain registration. Its size must be " + strprintf("%d", SC_PROOF_SIZE) + "bytes \n"
            "      \"vFieldElementCertificateField\":\"field els\"     (array, optional) An array of HEX string... TODO add description\n"
            "      \"vBitVectorCertificateField\":\"cmp mkl trees\"  (array, optional) An array of HEX string... TODO add description\n"
            "      \"ftScFee\"                         (numeric, optional) The Forward Transfer sidechain fee\n"
            "      \"mbtrScFee\"                       (numeric, optional) The Mainchain Backward Transfer Request sidechain fee\n"
            "    }\n"
            "\nResult:\n"
            "\"certificate\" (string) hex string of the certificate\n"

            "\nExamples\n"
            + HelpExampleCli("createrawcertificate",
                "\'[{\"txid\":\"7e3caf89f5f56fa7466f41d869d48c17ed8148a5fc6cc4c5923664dd2e667afe\", \"vout\": 0}]\' "
                "\'{\"ztmDWqXc2ZaMDGMhsgnVEmPKGLhi5GhsQok\":10.0}\' \'{\"fde10bda830e1d8590ca8bb8da8444cad953a852\":0.1}\' "
                "\'{\"scid\":\"02c5e79e8090c32e01e2a8636bfee933fd63c0cc15a78f0888cdf2c25b4a5e5f\", \"withdrawalEpochNumber\":3, \"quality\":10, \"endEpochBlockHash\":\"05ae..4d\",  \"endEpochCumScTxCommTreeRoot\":\"abcd..ef\", \"scProof\": \"abcd..ef\"}\'"
                )
        );

    LOCK(cs_main);
    RPCTypeCheck(params, boost::assign::list_of 
        (UniValue::VARR)(UniValue::VOBJ) (UniValue::VOBJ)(UniValue::VOBJ));

    UniValue inputs          = params[0].get_array();
    UniValue standardOutputs = params[1].get_obj();
    UniValue backwardOutputs = params[2].get_obj();
    UniValue cert_params     = params[3].get_obj();

    CMutableScCertificate rawCert;
    rawCert.nVersion = SC_CERT_VERSION;

    // inputs
    AddInputsToRawObject(rawCert, inputs);

    // outputs: there should be just one of them accounting for the change, but we do not prevent a vector of outputs
    AddOutputsToRawObject(rawCert, standardOutputs);

    // backward transfer outputs
    set<CBitcoinAddress> setAddress;
    vector<string> addrList = backwardOutputs.getKeys();
    BOOST_FOREACH(const string& name_, addrList)
    {
        uint160 pkeyValue;
        pkeyValue.SetHex(name_);

        CKeyID keyID(pkeyValue);
        CBitcoinAddress address(keyID);
        if (!address.IsValid())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("Invalid Horizen address: ")+name_);

        if (setAddress.count(address))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, duplicated address: ")+name_);
        setAddress.insert(address);

        CScript scriptPubKey = GetScriptForDestination(address.Get(), false);
        CAmount nAmount = AmountFromValue(backwardOutputs[name_]);

        rawCert.addBwt(CTxOut(nAmount, scriptPubKey));
    }

    if (!cert_params.isObject())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected object");

    // keywords set in cmd
    std::set<std::string> setKeyArgs;

    // valid input keywords for certificate data
    static const std::set<std::string> validKeyArgs = {
        "scid", "withdrawalEpochNumber", "quality", "endEpochBlockHash", "endEpochCumScTxCommTreeRoot", "scProof",
        "vFieldElementCertificateField", "vBitVectorCertificateField", "ftScFee", "mbtrScFee"};

    // sanity check, report error if unknown/duplicate key-value pairs
    for (const string& s : cert_params.getKeys())
    {
        if (!validKeyArgs.count(s))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, unknown key: ") + s);

        if (setKeyArgs.count(s))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Duplicate key in input: ") + s);

        setKeyArgs.insert(s);
    }

    uint256 scId;
    if (setKeyArgs.count("scid"))
    {
        string inputString = find_value(cert_params, "scid").get_str();
        scId.SetHex(inputString);
    }
    else
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing mandatory parameter in input: \"scid\"" );
    }

    int withdrawalEpochNumber = -1;
    if (setKeyArgs.count("withdrawalEpochNumber"))
    {
        withdrawalEpochNumber = find_value(cert_params, "withdrawalEpochNumber").get_int();
    }
    else
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing mandatory parameter in input: \"withdrawalEpochNumber\"" );
    }

    int64_t quality;
    if (setKeyArgs.count("quality"))
    {
        quality = find_value(cert_params, "quality").get_int64();
        if (quality < 0)
        {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter \"quality\": must be a positive number");
        }
    }
    else
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing mandatory parameter in input: \"quality\"" );
    }

    // TODO - endEpochBlockHash will disappear as soon as we will have a working interface for the proof verification
    uint256 endEpochBlockHash;
    if (setKeyArgs.count("endEpochBlockHash"))
    {
        string inputString = find_value(cert_params, "endEpochBlockHash").get_str();
        endEpochBlockHash.SetHex(inputString);
    }
    else
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing mandatory parameter in input: \"endEpochBlockHash\"" );
    }

    CFieldElement endEpochCumScTxCommTreeRoot;
    if (setKeyArgs.count("endEpochCumScTxCommTreeRoot"))
    {
        string inputString = find_value(cert_params, "endEpochCumScTxCommTreeRoot").get_str();
        std::vector<unsigned char> aByteArray {};
        std::string errorStr;
        // check only size upper limit
        static const bool ENFORCE_STRICT_SIZE = false;
        if (!Sidechain::AddScData(inputString, aByteArray, CFieldElement::ByteSize(), ENFORCE_STRICT_SIZE, errorStr))
        {
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("end cum commitment tree root: ") + errorStr);
        }
        endEpochCumScTxCommTreeRoot = CFieldElement{aByteArray};
    }
    else
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing mandatory parameter in input: \"endEpochCumScTxCommTreeRoot\"" );
    }

    if (setKeyArgs.count("scProof"))
    {
        string inputString = find_value(cert_params, "scProof").get_str();
        std::string error;
        std::vector<unsigned char> scProofVec;
        if (!Sidechain::AddScData(inputString, scProofVec, SC_PROOF_SIZE, true, error))
            throw JSONRPCError(RPC_TYPE_ERROR, string("scProof: ") + error);

        libzendoomc::ScProof scProof(scProofVec);
        if (!libzendoomc::IsValidScProof(scProof))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "invalid cert \"scProof\"");
        
        rawCert.scProof = scProof;
    }
    else
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing mandatory parameter in input: \"scProof\"" );
    }

    CAmount ftScFee(0);

    if (setKeyArgs.count("ftScFee"))
    {
        ftScFee = AmountFromValue(find_value(cert_params, "ftScFee"));

        if (!MoneyRange(ftScFee))
        {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "ftScFee is not in a valid range" );
        }
    }

    CAmount mbtrScFee(0);

    if (setKeyArgs.count("mbtrScFee"))
    {
        mbtrScFee = AmountFromValue(find_value(cert_params, "mbtrScFee"));

        if (!MoneyRange(mbtrScFee))
        {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "mbtrScFee is not in a valid range" );
        }
    }

    // ---------------------------------------------------------
    // just check against a maximum size 
    static const size_t MAX_FE_SIZE_BYTES  = SC_FIELD_SIZE;
    if (setKeyArgs.count("vFieldElementCertificateField"))
    {
        UniValue feArray = find_value(cert_params, "vFieldElementCertificateField").get_array();

        int count = 0;
        for (const UniValue& o : feArray.getValues())
        {
            if (!o.isStr())
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected string");

            std::string errString;
            std::vector<unsigned char> fe;
            if (!Sidechain::AddCustomFieldElement(o.get_str(), fe, MAX_FE_SIZE_BYTES, errString))
                throw JSONRPCError(RPC_TYPE_ERROR, string("vFieldElementCertificateField[" + std::to_string(count) + "]") + errString);

            rawCert.vFieldElementCertificateField.push_back(fe);
            count++;
        }
    }

    // ---------------------------------------------------------
    // just check against a maximum size TODO for the time being set to 32 K
    static const size_t MAX_CMT_SIZE_BYTES = 1024*32;
    if (setKeyArgs.count("vBitVectorCertificateField"))
    {
        UniValue feArray = find_value(cert_params, "vBitVectorCertificateField").get_array();

        int count = 0;
        for (const UniValue& o : feArray.getValues())
        {
            if (!o.isStr())
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected string");

            std::string error;
            std::vector<unsigned char> cmt;
            if (!Sidechain::AddScData(o.get_str(), cmt, MAX_CMT_SIZE_BYTES, false, error))
                throw JSONRPCError(RPC_TYPE_ERROR, string("vBitVectorCertificateField[" + std::to_string(count) + "]") + error);

            rawCert.vBitVectorCertificateField.push_back(cmt);
            count++;
        }
    }

    rawCert.scId = scId;
    rawCert.epochNumber = withdrawalEpochNumber;
    rawCert.quality = quality;
    rawCert.endEpochBlockHash = endEpochBlockHash;
    rawCert.endEpochCumScTxCommTreeRoot = endEpochCumScTxCommTreeRoot;
    rawCert.forwardTransferScFee = ftScFee;
    rawCert.mainchainBackwardTransferRequestScFee = mbtrScFee;

    return EncodeHexCert(rawCert);
}


UniValue decoderawcertificate(const UniValue& params, bool fHelp)
{   
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "decoderawcertificate \"hexstring\"\n"

            "\nExamples:\n"
            + HelpExampleCli("decoderawcertificate", "\"hexstring\"")
            + HelpExampleRpc("decoderawcertificate", "\"hexstring\"")
        );

    LOCK(cs_main);
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR));

    CScCertificate cert;

    if (!DecodeHexCert(cert, params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");

    UniValue result(UniValue::VOBJ);
    CertToJSON(cert, uint256(), result);

    return result;
}

UniValue decodescript(const UniValue& params, bool fHelp)
{   
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "decodescript \"hex\"\n"
            "\nDecode a hex-encoded script.\n"
            "\nArguments:\n"
            "1. \"hex\"     (string) the hex encoded script\n"
            "\nResult:\n"
            "{\n"
            "  \"asm\":\"asm\",   (string) Script public key\n"
            "  \"hex\":\"hex\",   (string) hex encoded public key\n"
            "  \"type\":\"type\", (string) The output type\n"
            "  \"reqSigs\": n,    (numeric) The required signatures\n"
            "  \"addresses\": [   (json array of string)\n"
            "     \"address\"     (string) Zen address\n"
            "     ,...\n"
            "  ],\n"
            "  \"p2sh\",\"address\" (string) script address\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("decodescript", "\"hexstring\"")
            + HelpExampleRpc("decodescript", "\"hexstring\"")
        );

    LOCK(cs_main);
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR));

    UniValue r(UniValue::VOBJ);
    CScript script;
    if (params[0].get_str().size() > 0){
        vector<unsigned char> scriptData(ParseHexV(params[0], "argument"));
        script = CScript(scriptData.begin(), scriptData.end());
    } else {
        // Empty scripts are valid
    }
    ScriptPubKeyToJSON(script, r, false);

    r.push_back(Pair("p2sh", CBitcoinAddress(CScriptID(script)).ToString()));
    return r;
}

/** Pushes a JSON object for script verification or signing errors to vErrorsRet. */
static void TxInErrorToJSON(const CTxIn& txin, UniValue& vErrorsRet, const std::string& strMessage)
{
    UniValue entry(UniValue::VOBJ);
    entry.push_back(Pair("txid", txin.prevout.hash.ToString()));
    entry.push_back(Pair("vout", (uint64_t)txin.prevout.n));
    entry.push_back(Pair("scriptSig", HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
    entry.push_back(Pair("sequence", (uint64_t)txin.nSequence));
    entry.push_back(Pair("error", strMessage));
    vErrorsRet.push_back(entry);
}

/** Pushes a JSON object for script verification or signing errors to vErrorsRet. */
static void TxCswInErrorToJSON(const CTxCeasedSidechainWithdrawalInput& txcswin, int cswIndex, UniValue& vErrorsRet, const std::string& strMessage)
{
    UniValue entry(UniValue::VOBJ);
    entry.push_back(Pair("cswIndex", cswIndex));
    const CScript& sciptPubKey = txcswin.scriptPubKey();
    entry.push_back(Pair("scriptPubKey", HexStr(sciptPubKey.begin(), sciptPubKey.end())));
    entry.push_back(Pair("redeemScript", HexStr(txcswin.redeemScript.begin(), txcswin.redeemScript.end())));
    entry.push_back(Pair("error", strMessage));
    vErrorsRet.push_back(entry);
}

UniValue signrawcertificate(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "signrawcertificate \"hexstring\" ([\"privatekey1\",...] )\n"
            "\nSign inputs for raw certificate (serialized, hex-encoded).\n"
            "The second optional argument (may be null) is an array of base58-encoded private\n"
            "keys that, if given, will be the only keys used to sign the transaction.\n"
#ifdef ENABLE_WALLET
            + HelpRequiringPassphrase() + "\n"
#endif

            "\nArguments:\n"
            "1. \"hexstring\"     (string, required) The transaction hex string\n"
            "2. \"privatekeys\"     (string, optional) A json array of base58-encoded private keys for signing\n"
            "    [                  (json array of strings, or 'null' if none provided)\n"
            "      \"privatekey\"   (string) private key in base58-encoding\n"
            "      ,...\n"
            "    ]\n"

            "\nResult:\n"
            "{\n"
            "  \"hex\" : \"value\",           (string) The hex-encoded raw transaction with signature(s)\n"
            "  \"complete\" : true|false,   (boolean) If the transaction has a complete set of signatures\n"
            "  \"errors\" : [                 (json array of objects) Script verification errors (if there are any)\n"
            "    {\n"
            "      \"txid\" : \"hash\",           (string) The hash of the referenced, previous input transaction\n"
            "      \"vout\" : n,                (numeric) The index of the output to spent and used as input\n"
            "      \"scriptSig\" : \"hex\",       (string) The hex-encoded signature script\n"
            "      \"sequence\" : n,            (numeric) Script sequence number\n"
            "      \"error\" : \"text\"           (string) Verification or signing error related to the input\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("signrawtransaction", "\"myhex\"")
            + HelpExampleRpc("signrawtransaction", "\"myhex\"")
        );

#ifdef ENABLE_WALLET
    LOCK2(cs_main, pwalletMain ? &pwalletMain->cs_wallet : NULL);
#else
    LOCK(cs_main);
#endif
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR)(UniValue::VARR), true);

    vector<unsigned char> certData(ParseHexV(params[0], "argument 1"));
    CDataStream ssData(certData, SER_NETWORK, PROTOCOL_VERSION);

    if (ssData.empty())
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Missing input certificate");

    CMutableScCertificate certVariants;
    try {
        ssData >> certVariants;
    }
    catch (const std::exception&) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Cert decode failed");
    }

    if (!ssData.empty()) {
        // just one and only one certificate expected
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, strprintf("Found %d extra byte%safter certificate",
            ssData.size(), ssData.size()>1?"s ":" "));
    }

    // mergedCert will end up with all the signatures; it
    // starts as a clone of the rawcert:
    CMutableScCertificate mergedCert(certVariants);

    // Fetch previous transactions (inputs):
    CCoinsView viewDummy;
    CCoinsViewCache view(&viewDummy);
    {
        LOCK(mempool.cs);
        CCoinsViewCache &viewChain = *pcoinsTip;
        CCoinsViewMemPool viewMempool(&viewChain, mempool);
        view.SetBackend(viewMempool); // temporarily switch cache backend to db+mempool view

        BOOST_FOREACH(const CTxIn& txin, mergedCert.vin) {
            const uint256& prevHash = txin.prevout.hash;
            CCoins coins;
            view.AccessCoins(prevHash); // this is certainly allowed to fail
        }

        view.SetBackend(viewDummy); // switch back to avoid locking mempool for too long
    }

    bool fGivenKeys = false;
    CBasicKeyStore tempKeystore;
    if (params.size() > 1 && !params[1].isNull()) {
        fGivenKeys = true;
        UniValue keys = params[1].get_array();
        for (size_t idx = 0; idx < keys.size(); idx++) {
            UniValue k = keys[idx];
            CBitcoinSecret vchSecret;
            bool fGood = vchSecret.SetString(k.get_str());
            if (!fGood)
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key");
            CKey key = vchSecret.GetKey();
            if (!key.IsValid())
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Private key outside allowed range");
            tempKeystore.AddKey(key);
        }
    }

    #ifdef ENABLE_WALLET
        EnsureWalletIsUnlocked();
        const CKeyStore& keystore = ((fGivenKeys || !pwalletMain) ? tempKeystore : *pwalletMain);
    #else
        const CKeyStore& keystore = tempKeystore;
    #endif

    int nHashType = SIGHASH_ALL;

    // Script verification errors
    UniValue vErrors(UniValue::VARR);

    // Sign what we can:
    for (unsigned int i = 0; i < mergedCert.vin.size(); i++) {
        CTxIn& txin = mergedCert.vin[i];
        const CCoins* coins = view.AccessCoins(txin.prevout.hash);
        if (coins == NULL || !coins->IsAvailable(txin.prevout.n)) {
            TxInErrorToJSON(txin, vErrors, "Input not found or already spent");
            continue;
        }
        const CScript& prevPubKey = coins->vout[txin.prevout.n].scriptPubKey;

        txin.scriptSig.clear();
        SignSignature(keystore, prevPubKey, mergedCert, i, nHashType);

        ScriptError serror = SCRIPT_ERR_OK;
        if (!VerifyScript(
                txin.scriptSig, prevPubKey, STANDARD_NONCONTEXTUAL_SCRIPT_VERIFY_FLAGS,
                MutableCertificateSignatureChecker(&mergedCert, i),
                &serror
           ))
        {
            TxInErrorToJSON(txin, vErrors, ScriptErrorString(serror));
        }
    }
    bool fComplete = vErrors.empty();

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("hex", EncodeHexCert(CScCertificate(mergedCert))));
    result.push_back(Pair("complete", fComplete));
    if (!vErrors.empty()) {
        result.push_back(Pair("errors", vErrors));
    }

    return result;
}

UniValue signrawtransaction(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 4)
        throw runtime_error(
            "signrawtransaction \"hexstring\" ( [{\"txid\":\"id\",\"vout\":n,\"scriptPubKey\":\"hex\",\"redeemScript\":\"hex\"},...] [\"privatekey1\",...] sighashtype )\n"
            "\nSign inputs for raw transaction (serialized, hex-encoded).\n"
            "The second optional argument (may be null) is an array of previous transaction outputs that\n"
            "this transaction depends on but may not yet be in the block chain.\n"
            "The third optional argument (may be null) is an array of base58-encoded private\n"
            "keys that, if given, will be the only keys used to sign the transaction.\n"
#ifdef ENABLE_WALLET
            + HelpRequiringPassphrase() + "\n"
#endif

            "\nArguments:\n"
            "1. \"hexstring\"     (string, required) The transaction hex string\n"
            "2. \"prevtxs\"       (string, optional) An json array of previous dependent transaction outputs\n"
            "     [               (json array of json objects, or 'null' if none provided)\n"
            "       {\n"
            "         \"txid\":\"id\",             (string, required) The transaction id\n"
            "         \"vout\":n,                  (numeric, required) The output number\n"
            "         \"scriptPubKey\": \"hex\",   (string, required) script key\n"
            "         \"redeemScript\": \"hex\"    (string, required for P2SH) redeem script\n"
            "       }\n"
            "       ,...\n"
            "    ]\n"
            "3. \"privatekeys\"     (string, optional) A json array of base58-encoded private keys for signing\n"
            "    [                  (json array of strings, or 'null' if none provided)\n"
            "      \"privatekey\"   (string) private key in base58-encoding\n"
            "      ,...\n"
            "    ]\n"
            "4. \"sighashtype\"     (string, optional, default=ALL) The signature hash type. Must be one of\n"
            "       \"ALL\"\n"
            "       \"NONE\"\n"
            "       \"SINGLE\"\n"
            "       \"ALL|ANYONECANPAY\"\n"
            "       \"NONE|ANYONECANPAY\"\n"
            "       \"SINGLE|ANYONECANPAY\"\n"

            "\nResult:\n"
            "{\n"
            "  \"hex\" : \"value\",           (string) The hex-encoded raw transaction with signature(s)\n"
            "  \"complete\" : true|false,   (boolean) If the transaction has a complete set of signatures\n"
            "  \"errors\" : [                 (json array of objects) Script verification errors (if there are any)\n"
            "    {\n"
            "      \"txid\" : \"hash\",           (string) The hash of the referenced, previous transaction\n"
            "      \"vout\" : n,                (numeric) The index of the output to spent and used as input\n"
            "      \"scriptSig\" : \"hex\",       (string) The hex-encoded signature script\n"
            "      \"sequence\" : n,            (numeric) Script sequence number\n"
            "      \"error\" : \"text\"           (string) Verification or signing error related to the input\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("signrawtransaction", "\"myhex\"")
            + HelpExampleRpc("signrawtransaction", "\"myhex\"")
        );

#ifdef ENABLE_WALLET
    LOCK2(cs_main, pwalletMain ? &pwalletMain->cs_wallet : NULL);
#else
    LOCK(cs_main);
#endif
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR)(UniValue::VARR)(UniValue::VARR)(UniValue::VSTR), true);

    vector<unsigned char> txData(ParseHexV(params[0], "argument 1"));
    CDataStream ssData(txData, SER_NETWORK, PROTOCOL_VERSION);
    vector<CMutableTransaction> txVariants;
    while (!ssData.empty()) {
        try {
            CMutableTransaction tx;
            ssData >> tx;
            txVariants.push_back(tx);
        }
        catch (const std::exception&) {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
        }
    }

    if (txVariants.empty())
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Missing transaction");

    // mergedTx will end up with all the signatures; it
    // starts as a clone of the rawtx:
    CMutableTransaction mergedTx(txVariants[0]);

    // Fetch previous transactions (inputs):
    CCoinsView viewDummy;
    CCoinsViewCache view(&viewDummy);
    {
        LOCK(mempool.cs);
        CCoinsViewCache &viewChain = *pcoinsTip;
        CCoinsViewMemPool viewMempool(&viewChain, mempool);
        view.SetBackend(viewMempool); // temporarily switch cache backend to db+mempool view

        BOOST_FOREACH(const CTxIn& txin, mergedTx.vin) {
            const uint256& prevHash = txin.prevout.hash;
            CCoins coins;
            view.AccessCoins(prevHash); // this is certainly allowed to fail
        }

        view.SetBackend(viewDummy); // switch back to avoid locking mempool for too long
    }

    bool fGivenKeys = false;
    CBasicKeyStore tempKeystore;
    if (params.size() > 2 && !params[2].isNull()) {
        fGivenKeys = true;
        UniValue keys = params[2].get_array();
        for (size_t idx = 0; idx < keys.size(); idx++) {
            UniValue k = keys[idx];
            CBitcoinSecret vchSecret;
            bool fGood = vchSecret.SetString(k.get_str());
            if (!fGood)
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key");
            CKey key = vchSecret.GetKey();
            if (!key.IsValid())
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Private key outside allowed range");
            tempKeystore.AddKey(key);
        }
    }
#ifdef ENABLE_WALLET
    else if (pwalletMain)
        EnsureWalletIsUnlocked();
#endif

    // Add previous txouts given in the RPC call:
    if (params.size() > 1 && !params[1].isNull()) {
        UniValue prevTxs = params[1].get_array();
        for (size_t idx = 0; idx < prevTxs.size(); idx++) {
            const UniValue& p = prevTxs[idx];
            if (!p.isObject())
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "expected object with {\"txid'\",\"vout\",\"scriptPubKey\"}");

            UniValue prevOut = p.get_obj();

            RPCTypeCheckObj(prevOut, boost::assign::map_list_of("txid", UniValue::VSTR)("vout", UniValue::VNUM)("scriptPubKey", UniValue::VSTR));

            uint256 txid = ParseHashO(prevOut, "txid");

            int nOut = find_value(prevOut, "vout").get_int();
            if (nOut < 0)
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "vout must be positive");

            vector<unsigned char> pkData(ParseHexO(prevOut, "scriptPubKey"));
            CScript scriptPubKey(pkData.begin(), pkData.end());

            {
                CCoinsModifier coins = view.ModifyCoins(txid);
                if (coins->IsAvailable(nOut) && coins->vout[nOut].scriptPubKey != scriptPubKey) {
                    string err("Previous output scriptPubKey mismatch:\n");
                    err = err + coins->vout[nOut].scriptPubKey.ToString() + "\nvs:\n"+
                        scriptPubKey.ToString();
                    throw JSONRPCError(RPC_DESERIALIZATION_ERROR, err);
                }
                if ((unsigned int)nOut >= coins->vout.size())
                    coins->vout.resize(nOut+1);
                coins->vout[nOut].scriptPubKey = scriptPubKey;
                coins->vout[nOut].nValue = 0; // we don't know the actual output value
            }

            // if redeemScript given and not using the local wallet (private keys
            // given), add redeemScript to the tempKeystore so it can be signed:
            if (fGivenKeys && scriptPubKey.IsPayToScriptHash()) {
                RPCTypeCheckObj(prevOut, boost::assign::map_list_of("txid", UniValue::VSTR)("vout", UniValue::VNUM)("scriptPubKey", UniValue::VSTR)("redeemScript",UniValue::VSTR));
                UniValue v = find_value(prevOut, "redeemScript");
                if (!v.isNull()) {
                    vector<unsigned char> rsData(ParseHexV(v, "redeemScript"));
                    CScript redeemScript(rsData.begin(), rsData.end());
                    tempKeystore.AddCScript(redeemScript);
                }
            }
        }
    }

#ifdef ENABLE_WALLET
    const CKeyStore& keystore = ((fGivenKeys || !pwalletMain) ? tempKeystore : *pwalletMain);
#else
    const CKeyStore& keystore = tempKeystore;
#endif

    int nHashType = SIGHASH_ALL;
    if (params.size() > 3 && !params[3].isNull()) {
        static map<string, int> mapSigHashValues =
            boost::assign::map_list_of
            (string("ALL"), int(SIGHASH_ALL))
            (string("ALL|ANYONECANPAY"), int(SIGHASH_ALL|SIGHASH_ANYONECANPAY))
            (string("NONE"), int(SIGHASH_NONE))
            (string("NONE|ANYONECANPAY"), int(SIGHASH_NONE|SIGHASH_ANYONECANPAY))
            (string("SINGLE"), int(SIGHASH_SINGLE))
            (string("SINGLE|ANYONECANPAY"), int(SIGHASH_SINGLE|SIGHASH_ANYONECANPAY))
            ;
        string strHashType = params[3].get_str();
        if (mapSigHashValues.count(strHashType))
            nHashType = mapSigHashValues[strHashType];
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid sighash param");
    }

    bool fHashSingle = ((nHashType & ~SIGHASH_ANYONECANPAY) == SIGHASH_SINGLE);

#if 1 
    // TODO check this as per implementation in interpreter.cpp
    // prevent user from using SIGHASH_ANYONECANPAY and Ceased Sidechain Withdrawals Inputs
    //
    // If we didn't check it here the cmd would return with an error anyway because the check on VerifyScript()
    // below for csws would fail. We choose to abort here the cmd.
    if ((nHashType & SIGHASH_ANYONECANPAY) && !mergedTx.vcsw_ccin.empty() )
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "can not have SIGHASH_ANYONECANPAY and csw input");
    }
#endif

    // Script verification errors
    UniValue vErrors(UniValue::VARR);

    // Sign what we can:
    for (unsigned int i = 0; i < mergedTx.vin.size(); i++) {
        CTxIn& txin = mergedTx.vin[i];
        const CCoins* coins = view.AccessCoins(txin.prevout.hash);
        if (coins == NULL || !coins->IsAvailable(txin.prevout.n)) {
            TxInErrorToJSON(txin, vErrors, "Input not found or already spent");
            continue;
        }
        const CScript& prevPubKey = coins->vout[txin.prevout.n].scriptPubKey;

        txin.scriptSig.clear();
        // Only sign SIGHASH_SINGLE if there's a corresponding output:
        if (!fHashSingle || (i < mergedTx.getVout().size()))
            SignSignature(keystore, prevPubKey, mergedTx, i, nHashType);

        // ... and merge in other signatures:
        BOOST_FOREACH(const CMutableTransaction& txv, txVariants) {
            txin.scriptSig = CombineSignatures(prevPubKey, mergedTx, i, txin.scriptSig, txv.vin[i].scriptSig);
        }
        ScriptError serror = SCRIPT_ERR_OK;
        if (!VerifyScript(txin.scriptSig, prevPubKey, STANDARD_NONCONTEXTUAL_SCRIPT_VERIFY_FLAGS, MutableTransactionSignatureChecker(&mergedTx, i), &serror)) {
            TxInErrorToJSON(txin, vErrors, ScriptErrorString(serror));
        }
    }

    if(mergedTx.IsScVersion())
    {
        // Try to sign CeasedSidechainWithdrawal inputs:
        unsigned int nAllInputsIndex = mergedTx.vin.size();
        for (unsigned int i = 0; i < mergedTx.vcsw_ccin.size(); i++, nAllInputsIndex++)
        {
            CTxCeasedSidechainWithdrawalInput& txCswIn = mergedTx.vcsw_ccin[i];

            const CScript& prevPubKey = txCswIn.scriptPubKey();

            txCswIn.redeemScript.clear();
            // Only sign SIGHASH_SINGLE if there's a corresponding output:
            // Note: we should consider the regular inputs as well.
            if (!fHashSingle || (nAllInputsIndex < mergedTx.getVout().size()))
                SignSignature(keystore, prevPubKey, mergedTx, nAllInputsIndex, nHashType);

            // ... and merge in other signatures:
            /* Note:
             * For CTxCeasedSidechainWithdrawalInput currently only P2PKH is allowed.
             * SignSignature can return true and set `txCswIn.redeemScript` value in case there is a proper private key in the keystore.
             * It can return false and leave `txCswIn.redeemScript` empty in case of any error occurs.
             * CombineSignatures will try to get the most recent signature:
             * 1) if SignSignature operation was successful -> leave `txCswIn.redeemScript value as is.
             * 2) if SignSignature operation was unsuccessful -> set `txCswIn.redeemScript value equal to the origin `txv` csw input script.
             * Later the signature will be checked, so in case no origin signature and no new one exist -> verification will fail.
             */
            for(const CMutableTransaction& txv : txVariants)
                txCswIn.redeemScript = CombineSignatures(prevPubKey, mergedTx, nAllInputsIndex, txCswIn.redeemScript, txv.vcsw_ccin[i].redeemScript);

            ScriptError serror = SCRIPT_ERR_OK;
            if (!VerifyScript(txCswIn.redeemScript, prevPubKey, STANDARD_NONCONTEXTUAL_SCRIPT_VERIFY_FLAGS,
                              MutableTransactionSignatureChecker(&mergedTx, nAllInputsIndex), &serror))
            {
                TxCswInErrorToJSON(txCswIn, i, vErrors, ScriptErrorString(serror));
            }
        }
    }

    bool fComplete = vErrors.empty();

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("hex", EncodeHexTx(CTransaction(mergedTx))));
    result.push_back(Pair("complete", fComplete));
    if (!vErrors.empty()) {
        result.push_back(Pair("errors", vErrors));
    }

    return result;
}

UniValue sendrawtransaction(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "sendrawtransaction \"hexstring\" ( allowhighfees )\n"
            "\nSubmits raw transaction (serialized, hex-encoded) to local node and network.\n"
            "\nAlso see createrawtransaction and signrawtransaction calls.\n"
            "\nArguments:\n"
            "1. \"hexstring\"    (string, required) The hex string of the raw transaction)\n"
            "2. allowhighfees    (boolean, optional, default=false) Allow high fees\n"
            "\nResult:\n"
            "\"hex\"             (string) The transaction hash in hex\n"
            "\nExamples:\n"
            "\nCreate a transaction\n"
            + HelpExampleCli("createrawtransaction", "\"[{\\\"txid\\\" : \\\"mytxid\\\",\\\"vout\\\":0}]\" \"{\\\"myaddress\\\":0.01}\"") +
            "Sign the transaction, and get back the hex\n"
            + HelpExampleCli("signrawtransaction", "\"myhex\"") +
            "\nSend the transaction (signed hex)\n"
            + HelpExampleCli("sendrawtransaction", "\"signedhex\"") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("sendrawtransaction", "\"signedhex\"")
        );

    LOCK(cs_main);
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR)(UniValue::VBOOL));

    // parse hex string from parameter
    CTransaction tx;
    if (!DecodeHexTx(tx, params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    uint256 hashTx = tx.GetHash();

    bool fOverrideFees = false;
    if (params.size() > 1)
        fOverrideFees = params[1].get_bool();

    RejectAbsurdFeeFlag fRejectAbsurdFee = fOverrideFees? RejectAbsurdFeeFlag::OFF : RejectAbsurdFeeFlag::ON;

    CCoinsViewCache &view = *pcoinsTip;
    const CCoins* existingCoins = view.AccessCoins(hashTx);
    bool fHaveMempool = mempool.exists(hashTx);
    bool fHaveChain = existingCoins && existingCoins->nHeight < 1000000000;
    if (!fHaveMempool && !fHaveChain) {
        // push to local node and sync with wallets
        CValidationState state;
        bool fMissingInputs;
        if (!AcceptTxToMemoryPool(mempool, state, tx, LimitFreeFlag::OFF, &fMissingInputs, fRejectAbsurdFee)) {
            if (state.IsInvalid()) {
                throw JSONRPCError(RPC_TRANSACTION_REJECTED, strprintf("%i: %s", CValidationState::CodeToChar(state.GetRejectCode()), state.GetRejectReason()));
            } else {
                if (fMissingInputs) {
                    throw JSONRPCError(RPC_TRANSACTION_ERROR, "Missing inputs");
                }
                throw JSONRPCError(RPC_TRANSACTION_ERROR, state.GetRejectReason());
            }
        }
    } else if (fHaveChain) {
        throw JSONRPCError(RPC_TRANSACTION_ALREADY_IN_CHAIN, "transaction already in block chain");
    }
    tx.Relay();

    return hashTx.GetHex();
}

UniValue sendrawcertificate(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "sendrawcertificate \"hexstring\" ( allowhighfees )\n"
            "\nSubmits raw certificate (serialized, hex-encoded) to local node and network.\n"
            "\nArguments:\n"
            "1. \"hexstring\"    (string, required) The hex string of the raw transaction)\n"
            "2. allowhighfees    (boolean, optional, default=false) Allow high fees\n"
            "\nResult:\n"
            "\"hex\"             (string) The transaction hash in hex\n"
            "\nExamples:\n"
            + HelpExampleCli("sendrawcertificate", "\"hex\"") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("sendrawcertificate", "\"hex\"")
        );

    LOCK(cs_main);
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR)(UniValue::VBOOL));

    // parse hex string from parameter
    CScCertificate cert;
    if (!DecodeHexCert(cert, params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Certificate decode failed");
    const uint256& hashCertificate = cert.GetHash();

    bool fOverrideFees = false;
    if (params.size() > 1)
    {
        fOverrideFees = params[1].get_bool();
    }
    RejectAbsurdFeeFlag fRejectAbsurdFee = fOverrideFees? RejectAbsurdFeeFlag::OFF : RejectAbsurdFeeFlag::ON;

    // check that we do not have it already somewhere
    CCoinsViewCache &view = *pcoinsTip;
    const CCoins* existingCoins = view.AccessCoins(hashCertificate);

    bool fHaveChain = existingCoins;
    bool fHaveMempool = mempool.existsCert(hashCertificate);

    if (!fHaveMempool && !fHaveChain)
    {
        // push to local node and sync with wallets
        CValidationState state;
        bool fMissingInputs;
        if (!AcceptCertificateToMemoryPool(mempool, state, cert, LimitFreeFlag::OFF, &fMissingInputs,
                fRejectAbsurdFee))
        {
            LogPrintf("%s():%d - cert[%s] not accepted in mempool\n", __func__, __LINE__, hashCertificate.ToString());
            if (state.IsInvalid())
            {
                throw JSONRPCError(RPC_TRANSACTION_REJECTED, strprintf("%i: %s", CValidationState::CodeToChar(state.GetRejectCode()), state.GetRejectReason()));
            }
            else
            {
                if (fMissingInputs)
                {
                    throw JSONRPCError(RPC_TRANSACTION_ERROR, "Missing inputs");
                }
                throw JSONRPCError(RPC_TRANSACTION_ERROR, "certificate not accepted to mempool");
            }
        }
    }
    else if (fHaveChain)
    {
        throw JSONRPCError(RPC_TRANSACTION_ALREADY_IN_CHAIN, "certificate already in block chain");
    }
    else
    {
        LogPrint("cert", "%s():%d - cert[%s] is already in mempool, just realying it\n", __func__, __LINE__, hashCertificate.ToString());
    }

    LogPrint("cert", "%s():%d - relaying certificate [%s]\n", __func__, __LINE__, hashCertificate.ToString());
    cert.Relay();

    return hashCertificate.GetHex();
}
