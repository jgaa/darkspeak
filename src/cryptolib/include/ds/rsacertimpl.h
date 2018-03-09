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
    enum class From {
        PUBLIC,
        PRIVATE
    };

    RsaCertImpl(const size_t bits);
    RsaCertImpl(const QByteArray& cert, const From from);
    ~RsaCertImpl();

    QByteArray getCert() const override;
    QByteArray getPubKey() const override;
    QByteArray getHash() const override;

private:
    void initPrivate(const QByteArray& cert);
    void initPublic(const QByteArray& pubkey);
    QByteArray getCertImpl(std::function<int (BIO *, RSA *)>) const;

    size_t bits_ = {};
    EVP_PKEY *key_ = {};
    //RSA *pub_key_= {};
};

}} // namespaces

#endif // RSACERTIMPL_H
