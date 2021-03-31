#include "sc/sidechaintypes.h"
#include "util.h"
#include <consensus/consensus.h>

void CZendooCctpLibraryChecker::CheckTypeSizes()
{
    if (SC_FIELD_SIZE+1 != zendoo_get_field_size_in_bytes())
    {
        LogPrintf("%s():%d - ERROR: unexpected CCTP field element size: %d (rust lib returns %d)\n", 
            __func__, __LINE__, SC_FIELD_SIZE, zendoo_get_field_size_in_bytes());
        assert(!"ERROR: field element size mismatch between rust CCTP lib and c header!");
    }
    if (SC_VK_SIZE != zendoo_get_sc_vk_size_in_bytes())
    {
        LogPrintf("%s():%d - ERROR: unexpected CCTP vk size: %d (rust lib returns %d)\n", 
            __func__, __LINE__, SC_VK_SIZE, zendoo_get_sc_vk_size_in_bytes());
        assert(!"ERROR: vk size mismatch between rust CCTP lib and c header!");
    }
    if (SC_PROOF_SIZE != zendoo_get_sc_proof_size_in_bytes())
    {
        LogPrintf("%s():%d - ERROR: unexpected CCTP proof size: %d (rust lib returns %d)\n", 
            __func__, __LINE__, SC_PROOF_SIZE, zendoo_get_sc_proof_size_in_bytes());
        assert(!"ERROR: proof size mismatch between rust CCTP lib and c header!");
    }
    if (SC_BV_SIZE_IN_BYTES != zendoo_get_sc_bit_vector_size_in_bytes())
    {
        LogPrintf("%s():%d - ERROR: unexpected CCTP bit vector size: %d (rust lib returns %d)\n", 
            __func__, __LINE__, SC_BV_SIZE_IN_BYTES, zendoo_get_sc_bit_vector_size_in_bytes());
        assert(!"ERROR: bit vector size mismatch between rust CCTP lib and c header!");
    }
    if (SC_CUSTOM_DATA_MAX_SIZE != zendoo_get_sc_custom_data_size_in_bytes())
    {
        LogPrintf("%s():%d - ERROR: unexpected CCTP custom data size: %d (rust lib returns %d)\n", 
            __func__, __LINE__, SC_CUSTOM_DATA_MAX_SIZE, zendoo_get_sc_custom_data_size_in_bytes());
        assert(!"ERROR: custom data size mismatch between rust CCTP lib and c header!");
    }
}

const std::vector<unsigned char>&  CZendooCctpObject::GetByteArray() const
{
    return byteVector;
}

const unsigned char* const CZendooCctpObject::GetDataBuffer() const
{
    if (GetByteArray().empty())
        return nullptr;

    return &GetByteArray()[0];
}

int CZendooCctpObject::GetDataSize() const
{
    return GetByteArray().size();
}

void CZendooCctpObject::SetNull() { byteVector.resize(0); }
bool CZendooCctpObject::IsNull() const { return byteVector.empty();}

std::string CZendooCctpObject::GetHexRepr() const
{
    std::string res; //ADAPTED FROM UTILSTRENCONDING.CPP HEXSTR
    static const char hexmap[16] = { '0', '1', '2', '3', '4', '5', '6', '7',
                                     '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
    res.reserve(this->byteVector.size()*2);
    for(const auto& byte: this->byteVector)
    {
        res.push_back(hexmap[byte>>4]);
        res.push_back(hexmap[byte&15]);
    }

    return res;
}

///////////////////////////////// Field types //////////////////////////////////
#ifdef BITCOIN_TX
CFieldElement::CFieldElement(const std::vector<unsigned char>& byteArrayIn) {};
void CFieldElement::SetByteArray(const std::vector<unsigned char>& byteArrayIn) {};
CFieldElement::CFieldElement(const uint256& value) {};
CFieldElement::CFieldElement(const wrappedFieldPtr& wrappedField) {};
wrappedFieldPtr CFieldElement::GetFieldElement() const {return nullptr;};
bool CFieldElement::IsValid() const {return false;};
CFieldElement CFieldElement::ComputeHash(const CFieldElement& lhs, const CFieldElement& rhs) { return CFieldElement{}; }
#else
CFieldElement::CFieldElement(const std::vector<unsigned char>& byteArrayIn): CZendooCctpObject(byteArrayIn)
{
    assert(byteArrayIn.size() == this->ByteSize());
}
void CFieldElement::SetByteArray(const std::vector<unsigned char>& byteArrayIn)
{
    assert(byteArrayIn.size() == this->ByteSize());
    this->byteVector = byteArrayIn;
}

CFieldElement::CFieldElement(const uint256& value)
{
    this->byteVector.resize(CFieldElement::ByteSize(),0x0);
    std::copy(value.begin(), value.end(), this->byteVector.begin());
}

CFieldElement::CFieldElement(const wrappedFieldPtr& wrappedField)
{
    this->byteVector.resize(CFieldElement::ByteSize(),0x0);
    if (wrappedField.get() != 0)
        zendoo_serialize_field(wrappedField.get(), &byteVector[0]);
}

wrappedFieldPtr CFieldElement::GetFieldElement() const
{
    if (this->byteVector.empty())
        return wrappedFieldPtr{nullptr};

    wrappedFieldPtr res = {zendoo_deserialize_field(&this->byteVector[0]), theFieldPtrDeleter};
    return res;
}

uint256 CFieldElement::GetLegacyHashTO_BE_REMOVED() const
{
    std::vector<unsigned char> tmp(this->byteVector.begin(), this->byteVector.begin()+32);
    return uint256(tmp);
}

bool CFieldElement::IsValid() const
{
    if(this->GetFieldElement() == nullptr)
        return false;

    return true;
}

CFieldElement CFieldElement::ComputeHash(const CFieldElement& lhs, const CFieldElement& rhs)
{
    if(!lhs.IsValid() || !rhs.IsValid())
        throw std::runtime_error("Could not compute poseidon hash on null field elements");

    ZendooPoseidonHash digest{};

    digest.update(lhs.GetFieldElement().get());
    digest.update(rhs.GetFieldElement().get());

    wrappedFieldPtr res = {digest.finalize(), theFieldPtrDeleter};
    return CFieldElement(res);
}

const CFieldElement& CFieldElement::GetPhantomHash()
{
    // TODO call an utility method to retrieve from zendoo_mc_cryptolib a constant phantom hash
    // field element and use it everywhere it is needed a constant value whose preimage has to
    // be unknown
    static CFieldElement ret{std::vector<unsigned char>(CFieldElement::ByteSize(),0x00)};
    return ret;
}
#endif
///////////////////////////// End of CFieldElement /////////////////////////////

/////////////////////////////////// CScProof ///////////////////////////////////
CScProof::CScProof(const std::vector<unsigned char>& byteArrayIn): CZendooCctpObject(byteArrayIn)
{
    assert(byteArrayIn.size() == this->ByteSize());
}

void CScProof::SetByteArray(const std::vector<unsigned char>& byteArrayIn)
{
    assert(byteArrayIn.size() == this->ByteSize());
    this->byteVector = byteArrayIn;
}

wrappedScProofPtr CScProof::GetProofPtr() const
{
    if (this->byteVector.empty())
        return wrappedScProofPtr{nullptr};

    wrappedScProofPtr res = {zendoo_deserialize_sc_proof(&this->byteVector[0]), theProofPtrDeleter};
    return res;
}

bool CScProof::IsValid() const
{
    if (this->GetProofPtr() == nullptr)
        return false;

    return true;
}
//////////////////////////////// End of CScProof ///////////////////////////////

//////////////////////////////////// CScVKey ///////////////////////////////////
CScVKey::CScVKey(const std::vector<unsigned char>& byteArrayIn): CZendooCctpObject(byteArrayIn)
{
    assert(byteArrayIn.size() == this->ByteSize());
}

void CScVKey::SetByteArray(const std::vector<unsigned char>& byteArrayIn)
{
    assert(byteArrayIn.size() == this->ByteSize());
    this->byteVector = byteArrayIn;
}

wrappedScVkeyPtr CScVKey::GetVKeyPtr() const
{
    if (this->byteVector.empty())
        return wrappedScVkeyPtr{nullptr};

    wrappedScVkeyPtr res = {zendoo_deserialize_sc_vk(&this->byteVector[0]), theVkPtrDeleter};
    return res;
}

bool CScVKey::IsValid() const
{
    if (this->GetVKeyPtr() == nullptr)
        return false;

    return true;
}

//////////////////////////////// End of CScVKey ////////////////////////////////

////////////////////////////// Custom Config types //////////////////////////////
bool FieldElementCertificateFieldConfig::IsValid() const
{
    if(nBits > 0 && nBits <= CFieldElement::ByteSize()*8)
        return true;
    else
        return false;
}

FieldElementCertificateFieldConfig::FieldElementCertificateFieldConfig(int32_t nBitsIn):
    CustomCertificateFieldConfig(), nBits(nBitsIn) {}

int32_t FieldElementCertificateFieldConfig::getBitSize() const
{
    return nBits;
}

//----------------------------------------------------------------------------------
bool BitVectorCertificateFieldConfig::IsValid() const
{
    bool isBitVectorSizeValid = (bitVectorSizeBits > 0) && (bitVectorSizeBits <= MAX_BIT_VECTOR_SIZE_BITS);
    if(!isBitVectorSizeValid)
        return false;

    if ((bitVectorSizeBits % 254 != 0) || (bitVectorSizeBits % 8 != 0))
        return false;

    bool isMaxCompressedSizeValid = (maxCompressedSizeBytes > 0) && (maxCompressedSizeBytes <= MAX_COMPRESSED_SIZE_BYTES);
    if(!isMaxCompressedSizeValid)
        return false;

    return true;
}

BitVectorCertificateFieldConfig::BitVectorCertificateFieldConfig(int32_t bitVectorSizeBits, int32_t maxCompressedSizeBytes):
    CustomCertificateFieldConfig(),
    bitVectorSizeBits(bitVectorSizeBits),
    maxCompressedSizeBytes(maxCompressedSizeBytes) {
    BOOST_STATIC_ASSERT(MAX_COMPRESSED_SIZE_BYTES <= MAX_CERT_SIZE); // sanity
}


////////////////////////////// Custom Field types //////////////////////////////


//----------------------------------------------------------------------------------------
FieldElementCertificateField::FieldElementCertificateField(const std::vector<unsigned char>& rawBytes)
    :CustomCertificateField(rawBytes), pReferenceCfg{nullptr} {}

FieldElementCertificateField::FieldElementCertificateField(const FieldElementCertificateField& rhs)
    :CustomCertificateField{}, pReferenceCfg{nullptr}
{
    *this = rhs;
}

FieldElementCertificateField& FieldElementCertificateField::operator=(const FieldElementCertificateField& rhs)
{
    *const_cast<std::vector<unsigned char>*>(&vRawData) = rhs.vRawData;
    if (rhs.pReferenceCfg != nullptr)
        this->pReferenceCfg = new FieldElementCertificateFieldConfig(*rhs.pReferenceCfg);
    else
        this->pReferenceCfg = nullptr;
    return *this;
}


bool FieldElementCertificateField::IsValid(const FieldElementCertificateFieldConfig& cfg) const
{
    return !this->GetFieldElement(cfg).IsNull();
}

const CFieldElement& FieldElementCertificateField::GetFieldElement(const FieldElementCertificateFieldConfig& cfg) const
{
    if (state != VALIDATION_STATE::NOT_INITIALIZED)
    {
        assert(pReferenceCfg != nullptr);
        if (*pReferenceCfg == cfg)
        {
            return fieldElement;
        }

        // revalidated with new cfg
        delete this->pReferenceCfg;
        this->pReferenceCfg = nullptr;
    }

    state = VALIDATION_STATE::INVALID;
    this->fieldElement = CFieldElement{};
    this->pReferenceCfg = new FieldElementCertificateFieldConfig(cfg);

    int rem = 0;

    assert(cfg.getBitSize() <= CFieldElement::BitSize());

    int bytes = getBytesFromBits(cfg.getBitSize(), rem);

    if (vRawData.size() != bytes )
    {
        LogPrint("sc", "%s():%d - ERROR: wrong size: data[%d] != cfg[%d]\n",
            __func__, __LINE__, vRawData.size(), cfg.getBitSize());
        return fieldElement;
    }

    if (rem)
    {
        // check null bits in the last byte are as expected
        unsigned char lastByte = vRawData.back();
        int numbOfZeroBits = getTrailingZeroBitsInByte(lastByte);
        if (numbOfZeroBits < (CHAR_BIT - rem))
        {
            LogPrint("sc", "%s():%d - ERROR: wrong number of null bits in last byte[0x%x]: %d vs %d\n",
                __func__, __LINE__, lastByte, numbOfZeroBits, (CHAR_BIT - rem));
            return fieldElement;
        }
    }

    std::vector<unsigned char> extendedRawData = vRawData;
    extendedRawData.insert(extendedRawData.begin(), CFieldElement::ByteSize()-vRawData.size(), 0x0);

    fieldElement.SetByteArray(extendedRawData);
    if (fieldElement.IsValid())
    {
        state = VALIDATION_STATE::VALID;
    } else
    {
        fieldElement = CFieldElement{};
    }

    return fieldElement;
}

//----------------------------------------------------------------------------------
BitVectorCertificateField::BitVectorCertificateField(const std::vector<unsigned char>& rawBytes)
    :CustomCertificateField(rawBytes), pReferenceCfg{nullptr} {}

BitVectorCertificateField::BitVectorCertificateField(const BitVectorCertificateField& rhs)
    :CustomCertificateField(), pReferenceCfg{nullptr}
{
    *this = rhs;
}

BitVectorCertificateField& BitVectorCertificateField::operator=(const BitVectorCertificateField& rhs)
{
    *const_cast<std::vector<unsigned char>*>(&vRawData) = rhs.vRawData;
    if (rhs.pReferenceCfg != nullptr)
        this->pReferenceCfg = new BitVectorCertificateFieldConfig(*rhs.pReferenceCfg);
    else
        this->pReferenceCfg = nullptr;
    return *this;
}

bool BitVectorCertificateField::IsValid(const BitVectorCertificateFieldConfig& cfg) const
{
    return !this->GetFieldElement(cfg).IsNull();
}

const CFieldElement& BitVectorCertificateField::GetFieldElement(const BitVectorCertificateFieldConfig& cfg) const
{
    if (state != VALIDATION_STATE::NOT_INITIALIZED)
    {
        assert(pReferenceCfg != nullptr);
        if (*pReferenceCfg == cfg)
        {
            return fieldElement;
        }

        // revalidated with new cfg
        delete this->pReferenceCfg;
        this->pReferenceCfg = nullptr;
    }

    state = VALIDATION_STATE::INVALID;
    this->pReferenceCfg = new BitVectorCertificateFieldConfig(cfg);

    if(vRawData.size() > cfg.getMaxCompressedSizeBytes()) {
        // this is invalid and fieldElement is Null 
        this->fieldElement = CFieldElement{};
        return fieldElement;
    }

    // Reconstruct MerkleTree from the compressed raw data of vRawField
    CctpErrorCode ret_code = CctpErrorCode::OK;
    BufferWithSize compressedData(&vRawData[0], vRawData.size());

    // the second parameter is the expected size of the uncompressed data. If this size is not matched the function returns
    // an error and a null filed element ptr
    field_t* fe = zendoo_merkle_root_from_compressed_bytes(&compressedData, SC_BV_SIZE_IN_BYTES, &ret_code);
    if (fe == nullptr)
    {
        LogPrint("sc", "%s():%d - ERROR(%d): could not get merkle root field el from compr bit vector of size %d, exp uncompr size %d\n",
            __func__, __LINE__, (int)ret_code, vRawData.size(), SC_BV_SIZE_IN_BYTES);
        this->fieldElement = CFieldElement{};
        return fieldElement;
    }
    this->fieldElement = CFieldElement{wrappedFieldPtr{fe, CFieldPtrDeleter{}}};
    state = VALIDATION_STATE::VALID;

    return fieldElement;
}

////////////////////////// End of Custom Field types ///////////////////////////
