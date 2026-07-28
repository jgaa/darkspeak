#ifndef CERTIMPL_H
#define CERTIMPL_H

#ifndef CertImpl_H
#define CertImpl_H

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
    ~CertImpl() override = default;

    const safe_array_t& getCert() const override;
    const safe_view_t& getSigningKey() const override;
    const safe_view_t& getSigningPubKey() const override;
    const safe_view_t& getEncryptionKey() const override;
    const safe_view_t& getEncryptionPubKey() const override;
    const safe_array_t& getHash() const override;

private:
    void CalculateHash();
    void init(bool privateKeys = true);
    void deriveCryptoKeys(bool deriveKey);

    safe_array_t cert_;
    // Public certificates are serialized as their signing public key only.
    // The encryption public key is derived from it and kept in cert_'s
    // internal public-key storage.
    safe_array_t serializedCert_;
    bool privateKeys_{true};
    safe_view_t signKey_;
    safe_view_t signPubKey_;
    safe_view_t encrKey_;
    safe_view_t encrPubKey_;
    safe_array_t hash_;
};

}} // namespaces

#endif // CertImpl_H


#endif // CERTIMPL_H
