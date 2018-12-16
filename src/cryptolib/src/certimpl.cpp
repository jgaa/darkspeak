
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
    key_.fill(0, crypto_box_SECRETKEYBYTES);
    pubkey_.fill(0, crypto_box_PUBLICKEYBYTES);

    // Generate a private / public key pair
    crypto_box_keypair(
                reinterpret_cast<unsigned char *>(pubkey_.data()),
                reinterpret_cast<unsigned char *>(key_.data()));

    // The cert is the combination of the two
    cert_.append(key_);
    cert_.append(pubkey_);
    hash_.fill(0, crypto_generichash_BYTES);

    CalculateHash();
}

CertImpl::CertImpl(const QByteArray &cert, const What what)
{
    if (what == What::CERT) {
        if (cert.size() != (crypto_box_SECRETKEYBYTES + crypto_box_PUBLICKEYBYTES)) {
            throw runtime_error("Invalid cert size");
        }
        key_.append(cert.data(), crypto_box_SECRETKEYBYTES);
        pubkey_.append(cert.data() + crypto_box_SECRETKEYBYTES, crypto_box_PUBLICKEYBYTES);
        cert_ = cert;
    } else {
        if (cert.size() != crypto_box_PUBLICKEYBYTES) {
            throw runtime_error("Invalid pubkey size");
        }
        pubkey_ = cert;
    }

    CalculateHash();
}

CertImpl::~CertImpl()
{
    key_.fill(0, key_.size());
    cert_.fill(0, cert_.size());
}

void CertImpl::CalculateHash()
{
    // Calculate the fingerprint
    if (pubkey_.size() != crypto_box_PUBLICKEYBYTES) {
        throw runtime_error("Invalid size of pubkey");
    }

    crypto_generichash(
                reinterpret_cast<unsigned char *>(hash_.data()),
                static_cast<size_t>(hash_.size()),
                reinterpret_cast<const unsigned char *>(pubkey_.data()),
                static_cast<size_t>(pubkey_.size()),
                nullptr, 0);
}

const QByteArray& CertImpl::getCert() const
{
    return cert_;
}

const QByteArray &CertImpl::getKey() const
{
    return key_;
}

const QByteArray& CertImpl::getPubKey() const
{
   return pubkey_;
}


const QByteArray& CertImpl::getHash() const
{
    return hash_;
}


DsCert::ptr_t DsCert::create() {
    return std::make_shared<CertImpl>();
}

DsCert::ptr_t DsCert::create(const QByteArray& cert) {
    return std::make_shared<CertImpl>(cert, CertImpl::What::CERT);
}

DsCert::ptr_t DsCert::createFromPubkey(const QByteArray& pubkey) {
    return  std::make_shared<CertImpl>(pubkey, CertImpl::What::PUBLIC);
}

}} // namespaces

