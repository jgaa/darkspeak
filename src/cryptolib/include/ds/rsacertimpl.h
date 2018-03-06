#ifndef RSACERTIMPL_H
#define RSACERTIMPL_H

#include <openssl/evp.h>
#include <openssl/rsa.h>

#include "ds/dscert.h"

namespace ds {
namespace crypto {

class RsaCertImpl : public DsCert
{
public:
    RsaCertImpl(const size_t bits);
    RsaCertImpl(const QByteArray& cert);
    ~RsaCertImpl();

    QByteArray getCert() const override;
    QByteArray getPubKey() const override;
    QByteArray getHash() const override;

private:
    QByteArray getCertImpl(std::function<int (BIO *, RSA *)>) const;

    size_t bits_ = {};
    EVP_PKEY *key_ = {};
};

}} // namespaces

#endif // RSACERTIMPL_H
