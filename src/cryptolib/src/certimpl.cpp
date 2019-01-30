
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

