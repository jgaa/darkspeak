
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
    crypto_sign_keypair(signPubKey_.data(), signKey_.data());
    deriveCryptoKeys(true);
    CalculateHash();
}

CertImpl::CertImpl(const safe_array_t &cert, const What what)
{
    if (what == What::CERT) {
        if (cert.size() != (crypto_sign_SECRETKEYBYTES + crypto_sign_PUBLICKEYBYTES
                          + crypto_box_SECRETKEYBYTES + crypto_box_PUBLICKEYBYTES)) {
            throw runtime_error("Invalid cert size");
        }

        init();
        cert_ = cert;
    } else {
        if (cert.size() != crypto_sign_PUBLICKEYBYTES) {
            throw runtime_error("Invalid pubkey size");
        }

        init();
        assert(cert.size() == signPubKey_.size());
        memcpy(signPubKey_.data(), cert.cdata(), signPubKey_.size());
        deriveCryptoKeys(false);
    }

    CalculateHash();
}

CertImpl::~CertImpl()
{
}

void CertImpl::CalculateHash()
{
    // Calculate the fingerprint
    if (signPubKey_.size() != crypto_sign_PUBLICKEYBYTES) {
        throw runtime_error("Invalid size of pubkey");
    }

    hash_.resize(crypto_generichash_BYTES);

    crypto_generichash(hash_.data(),hash_.size(),
                       signPubKey_.data(), signPubKey_.size(),
                       nullptr, 0);

    assert(!hash_.empty());
}

void CertImpl::init()
{
    hash_.resize(crypto_generichash_BYTES);
    cert_.resize(crypto_sign_SECRETKEYBYTES + crypto_sign_PUBLICKEYBYTES
                 + crypto_box_SECRETKEYBYTES + crypto_box_PUBLICKEYBYTES);

    // The cert is the combination of the key and pubkey
    // We share the memory in the certs buffer-space.
    signKey_.assign(cert_.data(), crypto_sign_SECRETKEYBYTES);
    signPubKey_.assign(signKey_.end(), crypto_sign_PUBLICKEYBYTES);
    encrKey_.assign(signPubKey_.end(), crypto_box_SECRETKEYBYTES);
    encrPubKey_.assign(encrKey_.end(), crypto_box_PUBLICKEYBYTES);
}

void CertImpl::deriveCryptoKeys(bool deriveKey)
{
    if (deriveKey) {
        if (crypto_sign_ed25519_sk_to_curve25519(encrKey_.data(), signKey_.cdata()) != 0) {
            throw runtime_error("Failed to derive crypto key");
        }
    }

    if (crypto_sign_ed25519_pk_to_curve25519(encrPubKey_.data(), signPubKey_.cdata()) != 0) {
        throw runtime_error("Failed to derive crypto pubkey");
    }
}

const DsCert::safe_array_t& CertImpl::getCert() const
{
    return cert_;
}

const DsCert::safe_view_t &CertImpl::getSigningKey() const
{
    return signKey_;
}

const DsCert::safe_view_t& CertImpl::getSigningPubKey() const
{
   return signPubKey_;
}

const DsCert::safe_view_t &CertImpl::getEncryptionKey() const
{
    return signKey_;
}

const DsCert::safe_view_t& CertImpl::getEncryptionPubKey() const
{
   return encrPubKey_;
}


const DsCert::safe_array_t& CertImpl::getHash() const
{
    return hash_;
}

DsCert::ptr_t DsCert::create() {
    return std::make_shared<CertImpl>();
}

DsCert::ptr_t DsCert::create(const safe_array_t& cert) {
    return std::make_shared<CertImpl>(cert, CertImpl::What::CERT);
}

// TODO: Remove when we encrypt secrets in the database
DsCert::ptr_t DsCert::create(const QByteArray& cert) {
    safe_array_t crt{cert};
    return create(crt);
}

DsCert::ptr_t DsCert::createFromPubkey(const safe_array_t& pubkey) {
    return  std::make_shared<CertImpl>(pubkey, CertImpl::What::PUBLIC);
}

// TODO: Remove when we encrypt secrets in the database
DsCert::ptr_t DsCert::createFromPubkey(const QByteArray& pubkey) {
    safe_array_t pk{pubkey};
    return createFromPubkey(pk);
}

}} // namespaces

