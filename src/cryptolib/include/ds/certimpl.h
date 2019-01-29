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
    CertImpl(const safe_array_t& cert, const What what);
    ~CertImpl() override;

    const safe_array_t& getCert() const override;
    const safe_view_t& getKey() const override;
    const safe_view_t& getPubKey() const override;
    const safe_array_t& getHash() const override;

//    safe_array_t sign(std::initializer_list<const safe_view_t> data) const override;
//    bool verify(const safe_array_t& signature,
//                std::initializer_list<const safe_view_t> data) const override;
private:
    void CalculateHash();
    void init();

    safe_array_t cert_;
    safe_view_t key_;
    safe_view_t pubkey_;
    safe_array_t hash_;
};

}} // namespaces

#endif // CertImpl_H


#endif // CERTIMPL_H
