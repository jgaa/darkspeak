#ifndef CERTIMPL_H
#define CERTIMPL_H

#ifndef CertImpl_H
#define CertImpl_H

#include <openssl/evp.h>
#include <openssl/rsa.h>

#include "ds/dscert.h"

namespace ds {
namespace crypto {

class CertImpl : public DsCert
{
public:
    enum class What {
        PUBLIC, // Just a pubkey
        CERT // Private / public key pair
    };

    CertImpl();
    CertImpl(const QByteArray& cert, const What what);
    ~CertImpl() override;

    const QByteArray& getCert() const override;
    const QByteArray& getKey() const override;
    const QByteArray& getPubKey() const override;
    const QByteArray& getHash() const override;

    QByteArray sign(std::initializer_list<QByteArray> data) const override;
    bool verify(const QByteArray& signature,
                std::initializer_list<QByteArray> data) const override;
private:
    void CalculateHash();

    QByteArray cert_;
    QByteArray key_;
    QByteArray pubkey_;
    QByteArray hash_;
};

}} // namespaces

#endif // CertImpl_H


#endif // CERTIMPL_H
