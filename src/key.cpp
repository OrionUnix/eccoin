/*
 * This file is part of the Eccoin project
 * Copyright (c) 2009-2017 The Bitcoin Core developers
 * Copyright (c) 2015-2017 The Bitcoin Unlimited developers
 * Copyright (c) 2014-2018 The Eccoin developers
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "key.h"

#include "arith_uint256.h"
#include "crypto/common.h"
#include "crypto/hmac_sha512.h"
#include "pubkey.h"
#include "random.h"

#include <secp256k1.h>
#include <secp256k1_recovery.h>

static secp256k1_context *secp256k1_context_sign = nullptr;


/** These functions are taken from the libsecp256k1 distribution and are very ugly. */
static int ec_privkey_import_der(const secp256k1_context *ctx,
    unsigned char *out32,
    const unsigned char *privkey,
    size_t privkeylen)
{
    const unsigned char *end = privkey + privkeylen;
    int lenb = 0;
    int len = 0;
    memset(out32, 0, 32);
    /* sequence header */
    if (end < privkey + 1 || *privkey != 0x30)
    {
        return 0;
    }
    privkey++;
    /* sequence length constructor */
    if (end < privkey + 1 || !(*privkey & 0x80))
    {
        return 0;
    }
    lenb = *privkey & ~0x80;
    privkey++;
    if (lenb < 1 || lenb > 2)
    {
        return 0;
    }
    if (end < privkey + lenb)
    {
        return 0;
    }
    /* sequence length */
    len = privkey[lenb - 1] | (lenb > 1 ? privkey[lenb - 2] << 8 : 0);
    privkey += lenb;
    if (end < privkey + len)
    {
        return 0;
    }
    /* sequence element 0: version number (=1) */
    if (end < privkey + 3 || privkey[0] != 0x02 || privkey[1] != 0x01 || privkey[2] != 0x01)
    {
        return 0;
    }
    privkey += 3;
    /* sequence element 1: octet string, up to 32 bytes */
    if (end < privkey + 2 || privkey[0] != 0x04 || privkey[1] > 0x20 || end < privkey + 2 + privkey[1])
    {
        return 0;
    }
    memcpy(out32 + 32 - privkey[1], privkey + 2, privkey[1]);
    if (!secp256k1_ec_seckey_verify(ctx, out32))
    {
        memset(out32, 0, 32);
        return 0;
    }
    return 1;
}

static int ec_privkey_export_der(const secp256k1_context *ctx,
    unsigned char *privkey,
    size_t *privkeylen,
    const unsigned char *key32,
    int compressed)
{
    secp256k1_pubkey pubkey;
    size_t pubkeylen = 0;
    if (!secp256k1_ec_pubkey_create(ctx, &pubkey, key32))
    {
        *privkeylen = 0;
        return 0;
    }
    if (compressed)
    {
        static const unsigned char begin[] = {0x30, 0x81, 0xD3, 0x02, 0x01, 0x01, 0x04, 0x20};
        static const unsigned char middle[] = {0xA0, 0x81, 0x85, 0x30, 0x81, 0x82, 0x02, 0x01, 0x01, 0x30, 0x2C, 0x06,
            0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x01, 0x01, 0x02, 0x21, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFE, 0xFF, 0xFF, 0xFC, 0x2F, 0x30, 0x06, 0x04, 0x01, 0x00, 0x04, 0x01, 0x07, 0x04, 0x21, 0x02,
            0x79, 0xBE, 0x66, 0x7E, 0xF9, 0xDC, 0xBB, 0xAC, 0x55, 0xA0, 0x62, 0x95, 0xCE, 0x87, 0x0B, 0x07, 0x02, 0x9B,
            0xFC, 0xDB, 0x2D, 0xCE, 0x28, 0xD9, 0x59, 0xF2, 0x81, 0x5B, 0x16, 0xF8, 0x17, 0x98, 0x02, 0x21, 0x00, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE, 0xBA, 0xAE, 0xDC,
            0xE6, 0xAF, 0x48, 0xA0, 0x3B, 0xBF, 0xD2, 0x5E, 0x8C, 0xD0, 0x36, 0x41, 0x41, 0x02, 0x01, 0x01, 0xA1, 0x24,
            0x03, 0x22, 0x00};
        unsigned char *ptr = privkey;
        memcpy(ptr, begin, sizeof(begin));
        ptr += sizeof(begin);
        memcpy(ptr, key32, 32);
        ptr += 32;
        memcpy(ptr, middle, sizeof(middle));
        ptr += sizeof(middle);
        pubkeylen = 33;
        secp256k1_ec_pubkey_serialize(ctx, ptr, &pubkeylen, &pubkey, SECP256K1_EC_COMPRESSED);
        ptr += pubkeylen;
        *privkeylen = ptr - privkey;
    }
    else
    {
        static const unsigned char begin[] = {0x30, 0x82, 0x01, 0x13, 0x02, 0x01, 0x01, 0x04, 0x20};
        static const unsigned char middle[] = {0xA0, 0x81, 0xA5, 0x30, 0x81, 0xA2, 0x02, 0x01, 0x01, 0x30, 0x2C, 0x06,
            0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x01, 0x01, 0x02, 0x21, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFE, 0xFF, 0xFF, 0xFC, 0x2F, 0x30, 0x06, 0x04, 0x01, 0x00, 0x04, 0x01, 0x07, 0x04, 0x41, 0x04,
            0x79, 0xBE, 0x66, 0x7E, 0xF9, 0xDC, 0xBB, 0xAC, 0x55, 0xA0, 0x62, 0x95, 0xCE, 0x87, 0x0B, 0x07, 0x02, 0x9B,
            0xFC, 0xDB, 0x2D, 0xCE, 0x28, 0xD9, 0x59, 0xF2, 0x81, 0x5B, 0x16, 0xF8, 0x17, 0x98, 0x48, 0x3A, 0xDA, 0x77,
            0x26, 0xA3, 0xC4, 0x65, 0x5D, 0xA4, 0xFB, 0xFC, 0x0E, 0x11, 0x08, 0xA8, 0xFD, 0x17, 0xB4, 0x48, 0xA6, 0x85,
            0x54, 0x19, 0x9C, 0x47, 0xD0, 0x8F, 0xFB, 0x10, 0xD4, 0xB8, 0x02, 0x21, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE, 0xBA, 0xAE, 0xDC, 0xE6, 0xAF, 0x48, 0xA0,
            0x3B, 0xBF, 0xD2, 0x5E, 0x8C, 0xD0, 0x36, 0x41, 0x41, 0x02, 0x01, 0x01, 0xA1, 0x44, 0x03, 0x42, 0x00};
        unsigned char *ptr = privkey;
        memcpy(ptr, begin, sizeof(begin));
        ptr += sizeof(begin);
        memcpy(ptr, key32, 32);
        ptr += 32;
        memcpy(ptr, middle, sizeof(middle));
        ptr += sizeof(middle);
        pubkeylen = 65;
        secp256k1_ec_pubkey_serialize(ctx, ptr, &pubkeylen, &pubkey, SECP256K1_EC_UNCOMPRESSED);
        ptr += pubkeylen;
        *privkeylen = ptr - privkey;
    }
    return 1;
}


int CompareBigEndian(const unsigned char *c1, size_t c1len, const unsigned char *c2, size_t c2len)
{
    while (c1len > c2len)
    {
        if (*c1)
            return 1;
        c1++;
        c1len--;
    }
    while (c2len > c1len)
    {
        if (*c2)
            return -1;
        c2++;
        c2len--;
    }
    while (c1len > 0)
    {
        if (*c1 > *c2)
            return 1;
        if (*c2 > *c1)
            return -1;
        c1++;
        c2++;
        c1len--;
    }
    return 0;
}

// Order of secp256k1's generator minus 1.
const unsigned char vchMaxModOrder[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFE, 0xBA, 0xAE, 0xDC, 0xE6, 0xAF, 0x48, 0xA0, 0x3B, 0xBF, 0xD2, 0x5E, 0x8C, 0xD0, 0x36, 0x41, 0x40};

// Half of the order of secp256k1's generator minus 1.
const unsigned char vchMaxModHalfOrder[32] = {0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0x5D, 0x57, 0x6E, 0x73, 0x57, 0xA4, 0x50, 0x1D, 0xDF, 0xE9, 0x2F, 0x46, 0x68, 0x1B, 0x20,
    0xA0};

const unsigned char vchZero[0] = {};


bool CKey::Check(const unsigned char *vch)
{
    // Do not convert to OpenSSL's data structures for range-checking keys,
    // it's easy enough to do directly.
    static const unsigned char vchMax[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFE, 0xBA, 0xAE, 0xDC, 0xE6, 0xAF, 0x48, 0xA0, 0x3B, 0xBF, 0xD2, 0x5E, 0x8C, 0xD0, 0x36,
        0x41, 0x40};
    bool fIsZero = true;
    for (int i = 0; i < 32 && fIsZero; i++)
        if (vch[i] != 0)
            fIsZero = false;
    if (fIsZero)
        return false;
    for (int i = 0; i < 32; i++)
    {
        if (vch[i] < vchMax[i])
            return true;
        if (vch[i] > vchMax[i])
            return false;
    }
    return true;
}

bool CKey::CheckSignatureElement(const unsigned char *vch, int len, bool half)
{
    return CompareBigEndian(vch, len, vchZero, 0) > 0 &&
           CompareBigEndian(vch, len, half ? vchMaxModHalfOrder : vchMaxModOrder, 32) <= 0;
}

const unsigned char vchOrder[32] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xfe, 0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b, 0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41};

const unsigned char vchHalfOrder[32] = {0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0x5d, 0x57, 0x6e, 0x73, 0x57, 0xa4, 0x50, 0x1d, 0xdf, 0xe9, 0x2f, 0x46, 0x68, 0x1b, 0x20, 0xa0};

void CKey::MakeNewKey(bool fCompressedIn)
{
    RandAddSeedPerfmon();
    do
    {
        GetRandBytes(vch, sizeof(vch));
    } while (!Check(vch));
    fValid = true;
    fCompressed = fCompressedIn;
}

bool CKey::SetPrivKey(const CPrivKey &privkey, bool fCompressedIn)
{
    if (!ec_privkey_import_der(secp256k1_context_sign, (unsigned char *)begin(), &privkey[0], privkey.size()))
        return false;
    fCompressed = fCompressedIn;
    fValid = true;
    return true;
}

CPrivKey CKey::GetPrivKey() const
{
    assert(fValid);
    CPrivKey privkey;
    int ret;
    size_t privkeylen;
    privkey.resize(279);
    privkeylen = 279;
    ret = ec_privkey_export_der(secp256k1_context_sign, (unsigned char *)&privkey[0], &privkeylen, begin(),
        fCompressed ? SECP256K1_EC_COMPRESSED : SECP256K1_EC_UNCOMPRESSED);
    assert(ret);
    privkey.resize(privkeylen);
    return privkey;
}

CPubKey CKey::GetPubKey() const
{
    assert(fValid);
    secp256k1_pubkey pubkey;
    size_t clen = 65;
    CPubKey result;
    int ret = secp256k1_ec_pubkey_create(secp256k1_context_sign, &pubkey, begin());
    assert(ret);
    secp256k1_ec_pubkey_serialize(secp256k1_context_sign, (unsigned char *)result.begin(), &clen, &pubkey,
        fCompressed ? SECP256K1_EC_COMPRESSED : SECP256K1_EC_UNCOMPRESSED);
    assert(result.size() == clen);
    assert(result.IsValid());
    return result;
}

bool CKey::Sign(const uint256 &hash, std::vector<unsigned char> &vchSig, uint32_t test_case) const
{
    if (!fValid)
        return false;
    vchSig.resize(72);
    size_t nSigLen = 72;
    unsigned char extra_entropy[32] = {0};
    WriteLE32(extra_entropy, test_case);
    secp256k1_ecdsa_signature sig;
    int ret = secp256k1_ecdsa_sign(secp256k1_context_sign, &sig, hash.begin(), begin(),
        secp256k1_nonce_function_rfc6979, test_case ? extra_entropy : NULL);
    assert(ret);
    secp256k1_ecdsa_signature_serialize_der(secp256k1_context_sign, (unsigned char *)&vchSig[0], &nSigLen, &sig);
    vchSig.resize(nSigLen);
    return true;
}

bool CKey::SignCompact(const uint256 &hash, std::vector<unsigned char> &vchSig) const
{
    if (!fValid)
        return false;
    vchSig.resize(65);
    int rec = -1;
    secp256k1_ecdsa_recoverable_signature sig;
    int ret = secp256k1_ecdsa_sign_recoverable(
        secp256k1_context_sign, &sig, hash.begin(), begin(), secp256k1_nonce_function_rfc6979, NULL);
    assert(ret);
    secp256k1_ecdsa_recoverable_signature_serialize_compact(
        secp256k1_context_sign, (unsigned char *)&vchSig[1], &rec, &sig);
    assert(ret);
    assert(rec != -1);
    vchSig[0] = 27 + rec + (fCompressed ? 4 : 0);
    return true;
}

bool CKey::Load(CPrivKey &privkey, CPubKey &vchPubKey, bool fSkipCheck = false)
{
    if (!ec_privkey_import_der(secp256k1_context_sign, (unsigned char *)begin(), &privkey[0], privkey.size()))
        return false;
    fCompressed = vchPubKey.IsCompressed();
    fValid = true;
    if (fSkipCheck)
        return true;
    return VerifyPubKey(vchPubKey);
}

bool CKey::Derive(CKey &keyChild, unsigned char ccChild[32], unsigned int nChild, const unsigned char cc[32]) const
{
    assert(IsValid());
    assert(IsCompressed());
    unsigned char out[64];
    LockObject(out);
    if ((nChild >> 31) == 0)
    {
        CPubKey pubkey = GetPubKey();
        assert(pubkey.begin() + 33 == pubkey.end());
        BIP32Hash(cc, nChild, *pubkey.begin(), pubkey.begin() + 1, out);
    }
    else
    {
        assert(begin() + 32 == end());
        BIP32Hash(cc, nChild, 0, begin(), out);
    }
    memcpy(ccChild, out + 32, 32);
    memcpy((unsigned char *)keyChild.begin(), begin(), 32);
    bool ret = secp256k1_ec_privkey_tweak_add(secp256k1_context_sign, (unsigned char *)keyChild.begin(), out);
    UnlockObject(out);
    keyChild.fCompressed = true;
    keyChild.fValid = ret;
    return ret;
}

bool CExtKey::Derive(CExtKey &out, unsigned int nChild) const
{
    out.nDepth = nDepth + 1;
    CKeyID id = key.GetPubKey().GetID();
    memcpy(&out.vchFingerprint[0], &id, 4);
    out.nChild = nChild;
    return key.Derive(out.key, out.vchChainCode, nChild, vchChainCode);
}

void CExtKey::SetMaster(const unsigned char *seed, unsigned int nSeedLen)
{
    static const unsigned char hashkey[] = {'B', 'i', 't', 'c', 'o', 'i', 'n', ' ', 's', 'e', 'e', 'd'};
    std::vector<unsigned char, secure_allocator<unsigned char> > vout(64);
    CHMAC_SHA512(hashkey, sizeof(hashkey)).Write(seed, nSeedLen).Finalize(vout.data());
    key.Set(vout.data(), vout.data() + 32, true);
    memcpy(vchChainCode, vout.data() + 32, 32);
    nDepth = 0;
    nChild = 0;
    memset(vchFingerprint, 0, sizeof(vchFingerprint));
}

CExtPubKey CExtKey::Neuter() const
{
    CExtPubKey ret;
    ret.nDepth = nDepth;
    memcpy(&ret.vchFingerprint[0], &vchFingerprint[0], 4);
    ret.nChild = nChild;
    ret.pubkey = key.GetPubKey();
    memcpy(&ret.vchChainCode[0], &vchChainCode[0], 32);
    return ret;
}

void CExtKey::Encode(unsigned char code[74]) const
{
    code[0] = nDepth;
    memcpy(code + 1, vchFingerprint, 4);
    code[5] = (nChild >> 24) & 0xFF;
    code[6] = (nChild >> 16) & 0xFF;
    code[7] = (nChild >> 8) & 0xFF;
    code[8] = (nChild >> 0) & 0xFF;
    memcpy(code + 9, vchChainCode, 32);
    code[41] = 0;
    assert(key.size() == 32);
    memcpy(code + 42, key.begin(), 32);
}

void CExtKey::Decode(const unsigned char code[74])
{
    nDepth = code[0];
    memcpy(vchFingerprint, code + 1, 4);
    nChild = (code[5] << 24) | (code[6] << 16) | (code[7] << 8) | code[8];
    memcpy(vchChainCode, code + 9, 32);
    key.Set(code + 42, code + 74, true);
}

bool ECC_InitSanityCheck()
{
    CKey key;
    key.MakeNewKey(true);
    CPubKey pubkey = key.GetPubKey();
    return key.VerifyPubKey(pubkey);
}

bool CKey::VerifyPubKey(const CPubKey &pubkey) const
{
    if (pubkey.IsCompressed() != fCompressed)
    {
        return false;
    }
    unsigned char rnd[8];
    std::string str = "Bitcoin key verification\n";
    GetRandBytes(rnd, sizeof(rnd));
    uint256 hash;
    CHash256().Write((unsigned char *)str.data(), str.size()).Write(rnd, sizeof(rnd)).Finalize(hash.begin());
    std::vector<unsigned char> vchSig;
    Sign(hash, vchSig);
    return pubkey.Verify(hash, vchSig);
}

void ECC_Start()
{
    assert(secp256k1_context_sign == nullptr);

    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    assert(ctx != nullptr);

    {
        // Pass in a random blinding seed to the secp256k1 context.
        std::vector<unsigned char, secure_allocator<unsigned char> > vseed(32);
        GetRandBytes(vseed.data(), 32);
        bool ret = secp256k1_context_randomize(ctx, vseed.data());
        assert(ret);
    }

    secp256k1_context_sign = ctx;
}

void ECC_Stop()
{
    secp256k1_context *ctx = secp256k1_context_sign;
    secp256k1_context_sign = nullptr;

    if (ctx)
    {
        secp256k1_context_destroy(ctx);
    }
}
