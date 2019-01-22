#ifndef DSCERT_H
#define DSCERT_H

#include <QByteArray>
#include <memory>

#include "base58.h"

namespace ds {
namespace crypto {

/*! Interface for a PKI 'cert' used by one  Identity.
 */
class DsCert {
public:
    struct Error : public std::runtime_error
    {
        Error(const char *what) : std::runtime_error(what) {}
    };

    using ptr_t = std::shared_ptr<DsCert>;

    virtual ~DsCert() = default;

    virtual const QByteArray& getCert() const = 0;
    virtual const QByteArray& getKey() const = 0;
    virtual const QByteArray& getPubKey() const = 0;
    virtual const QByteArray& getHash() const = 0;
    virtual const QByteArray getB58PubKey() {
        return b58check_enc<QByteArray>(getPubKey(), {249, 50});
    }

    // Sign data with the private key
    virtual QByteArray sign(std::initializer_list<QByteArray> data) const = 0;

    // Verify a signature using the public key
    virtual bool verify(const QByteArray& signature,
                        std::initializer_list<QByteArray> data) const = 0;

    /*! Factory to create a cert */
    static ptr_t create();
    static ptr_t create(const QByteArray& cert);
    static ptr_t createFromPubkey(const QByteArray& pubkey); // as returned by getPubKey()    
};

}} // namespaces

#endif // DSCERT_H
