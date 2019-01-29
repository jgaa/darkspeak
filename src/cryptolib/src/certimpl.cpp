
#include <assert.h>

#include <sodium.h>
#include <QDebug>

#include "ds/crypto.h"
#include "ds/cvar.h"
#include "ds/certimpl.h"

using namespace std;

namespace ds {
namespace crypto {

CertImpl::CertImpl()
{
    init();

    // Generate a private / public key pair
    crypto_box_keypair(pubkey_.data(), key_.data());
    CalculateHash();
}

CertImpl::CertImpl(const safe_array_t &cert, const What what)
{
    if (what == What::CERT) {
        if (cert.size() != (crypto_box_SECRETKEYBYTES + crypto_box_PUBLICKEYBYTES)) {
            throw runtime_error("Invalid cert size");
        }

        init();
        cert_ = cert;
    } else {
        if (cert.size() != crypto_box_PUBLICKEYBYTES) {
            throw runtime_error("Invalid pubkey size");
        }

        init();
        assert(cert.size() == pubkey_.size());
        memcpy(pubkey_.data(), cert.cdata(), pubkey_.size());
    }

    CalculateHash();
}

CertImpl::~CertImpl()
{
}

void CertImpl::CalculateHash()
{
    // Calculate the fingerprint
    if (pubkey_.size() != crypto_box_PUBLICKEYBYTES) {
        throw runtime_error("Invalid size of pubkey");
    }

    hash_.resize(crypto_generichash_BYTES);

    crypto_generichash(hash_.data(),hash_.size(),
                       pubkey_.data(), pubkey_.size(),
                       nullptr, 0);

    assert(!hash_.empty());
}

void CertImpl::init()
{
    hash_.resize(crypto_generichash_BYTES);
    cert_.resize(crypto_box_SECRETKEYBYTES + crypto_box_PUBLICKEYBYTES);

    // The cert is the combination of the key and pubkey
    // We share the memory in the certs buffer-space.
    key_.assign(cert_.data(), crypto_box_SECRETKEYBYTES);
    pubkey_.assign(key_.end(), crypto_box_PUBLICKEYBYTES);
}

const DsCert::safe_array_t& CertImpl::getCert() const
{
    return cert_;
}

const DsCert::safe_view_t &CertImpl::getKey() const
{
    return key_;
}

const DsCert::safe_view_t& CertImpl::getPubKey() const
{
   return pubkey_;
}


const DsCert::safe_array_t& CertImpl::getHash() const
{
    return hash_;
}

//DsCert::safe_array_t CertImpl::sign(std::initializer_list<const safe_view_t> data) const
//{
//    assert(!key_.empty());

//    safe_array_t signature(crypto_sign_BYTES);
//    crypto_sign_state state = {};

//    crypto_sign_init(&state);

//    for(const auto& d : data) {
//        crypto_sign_update(&state, d.cdata(), d.size());
//    }

//    crypto_sign_final_create(&state, signature.data(),
//                             nullptr, key_.cdata());

//    return signature;
//}


//bool CertImpl::verify(const safe_array_t &signature,
//                      std::initializer_list<const safe_view_t> data) const
//{
//    assert(!pubkey_.empty());

//    crypto_sign_state state = {};

//    crypto_sign_init(&state);

//    for(const auto& d : data) {
//        crypto_sign_update(&state, d.cdata(), d.size());
//    }

//    safe_array_t sign = signature; // libsodium wants non-const signature
//    const auto result = crypto_sign_final_verify(&state, sign.data(), pubkey_.cdata());

//    return result == 0;
//}

DsCert::ptr_t DsCert::create() {
    return std::make_shared<CertImpl>();
}

DsCert::ptr_t DsCert::create(const safe_array_t& cert) {
    return std::make_shared<CertImpl>(cert, CertImpl::What::CERT);
}

DsCert::ptr_t DsCert::createFromPubkey(const safe_array_t& pubkey) {
    return  std::make_shared<CertImpl>(pubkey, CertImpl::What::PUBLIC);
}

DsCert::ptr_t DsCert::createFromPubkey(const QByteArray& pubkey) {
    safe_array_t pk{pubkey};
    return createFromPubkey(pk);
}

}} // namespaces

